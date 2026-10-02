// 性能只报告；CRC、影子值、完成数量和 fd 回收才是通过判据。
#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>
#include <cstring>
#include <unistd.h>
#include "framework/ts.h"

using namespace ts;

TEST("stress.single_connection_qps", TAG_STRESS | TAG_BOARD | TAG_ASAN)
{
    ServerConfig cfg;
    cfg.asan = ctx.expectAsan;
    cfg.recyWait = 1;
    ServerFixture srv(cfg);
    ASSERT(ctx, srv.start(), "%s", srv.startup_error().c_str());
    ScopedConn c(srv);
    ASSERT(ctx, c.ok(), "connect failed");
    Rng rng(ctx.seed);
    const char* names[] = {fx::TAG_BIN256, fx::TAG_BIN4K, fx::TAG_BINMAX};
    const int sizes[] = {256, 4096, 16384};
    std::vector<char> shadow[3];
    unsigned err = 0;
    for (int t = 0; t < 3; ++t) {
        make_payload(shadow[t], sizes[t], 0, rng);
        REQUIRE_OK(ctx, writeb_notpost(c.fd(), names[t], shadow[t].data(), sizes[t], &err), err);
    }
    LatencyHistogram hist;
    const int n = 100000 * ctx.scale;
    hist.reserve(n);
    uint64_t start = now_nanos();
    for (int i = 0; i < n; ++i) {
        int bucket = rng.range(0, 99);
        int t = bucket < 95 ? 0 : (bucket < 99 ? 1 : 2);
        uint64_t at = now_nanos();
        if (rng.chance(45)) {
            make_payload(shadow[t], sizes[t], i + 1, rng);
            REQUIRE_OK(ctx, writeb_notpost(c.fd(), names[t], shadow[t].data(), sizes[t], &err), err);
        } else {
            std::vector<char> data(sizes[t]);
            REQUIRE_OK(ctx, readb(c.fd(), names[t], data.data(), sizes[t], &err, nullptr), err);
            std::string why;
            ASSERT(ctx, data == shadow[t] && check_payload(data.data(), sizes[t], -1, &why),
                   "iteration %d: shadow/CRC mismatch %s", i, why.c_str());
        }
        hist.record(now_nanos() - at);
    }
    hist.report("single-connection", (now_nanos() - start) / 1e9);
    CHECK(ctx, hist.count() == static_cast<size_t>(n), "incomplete workload");
}

TEST("stress.concurrent_connections", TAG_STRESS | TAG_BOARD | TAG_ASAN)
{
    ServerConfig cfg;
    cfg.asan = ctx.expectAsan;
    cfg.threads = 4;
    cfg.recyWait = 1;
    ServerFixture srv(cfg);
    ASSERT(ctx, srv.start(), "%s", srv.startup_error().c_str());
    constexpr int clients = 200;
    const int operations = 1000 * ctx.scale;
    std::vector<std::string> names;
    {
        ScopedConn admin(srv);
        ASSERT(ctx, admin.ok(), "admin connect failed");
        unsigned err = 0;
        char type = 1;
        for (int i = 0; i < clients; ++i) {
            names.push_back("BENCH_" + std::to_string(i));
            REQUIRE_OK(ctx, createtag(admin.fd(), names.back().c_str(), 256, &type, 1, &err), err);
        }
    }
    ProcSampler sampler(srv.worker_pid());
    ASSERT(ctx, sampler.tick(), "baseline sample failed");
    int baseline = sampler.fd_last();
    std::atomic<int> ready{0}, errors{0}, completed{0};
    std::atomic<bool> start{false};
    std::vector<LatencyHistogram> hist(clients);
    std::vector<std::thread> threads;
    try {
        for (int t = 0; t < clients; ++t) {
            threads.emplace_back([&, t] {
                ScopedConn conn(srv);
                if (!conn.ok()) { ++errors; ++ready; return; }
                ++ready;
                while (!start.load()) std::this_thread::yield();
                Rng rng(ctx.seed ^ static_cast<uint64_t>(t + 1));
                std::vector<char> expected, received(256);
                make_payload(expected, 256, 0, rng);
                unsigned err = 0;
                if (!writeb_notpost(conn.fd(), names[t].c_str(), expected.data(), 256, &err)) { ++errors; return; }
                for (int i = 0; i < operations; ++i) {
                    uint64_t at = now_nanos();
                    bool ok;
                    if (rng.chance(50)) {
                        make_payload(expected, 256, i + 1, rng);
                        ok = writeb_notpost(conn.fd(), names[t].c_str(), expected.data(), 256, &err);
                    } else {
                        ok = readb(conn.fd(), names[t].c_str(), received.data(), 256, &err, nullptr);
                        std::string why;
                        ok = ok && received == expected && check_payload(received.data(), 256, -1, &why);
                    }
                    if (!ok || err) { ++errors; break; }
                    hist[t].record(now_nanos() - at);
                    ++completed;
                }
            });
        }
    } catch (const std::exception& e) {
        ++errors;
        printf("    [error] thread creation: %s\n", e.what());
    }
    const uint64_t deadline = now_nanos() + 10000000000ull;
    while (ready.load() < static_cast<int>(threads.size()) && now_nanos() < deadline) usleep(2000);
    bool allReady = ready.load() == clients;
    bool sampled = false;
    int held = -1;
    // connect() 返回只代表握手完成（连接仍在 accept 队列），worker 的 accept 可能尚未追上；等待 fd 数到位。
    const uint64_t acceptDeadline = now_nanos() + 2000000000ull;
    do {
        sampled = sampler.tick();
        held = sampler.fd_last();
        if (!sampled || held >= baseline + clients - 2) break;
        usleep(1000);
    } while (now_nanos() < acceptDeadline);
    uint64_t at = now_nanos();
    start = true;
    for (auto& thread : threads) thread.join();
    double seconds = (now_nanos() - at) / 1e9;
    LatencyHistogram merged;
    for (int t = 0; t < clients; ++t) {
        // 各线程独占直方图；合并原始样本而非平均分位数。
        for (uint64_t ns : hist[t].samples()) merged.record(ns);
    }
    merged.report("200-connections", seconds);
    CHECK(ctx, allReady && errors == 0, "ready=%d/200 errors=%d", ready.load(), errors.load());
    CHECK(ctx, completed == clients * operations, "completed=%d expected=%d", completed.load(), clients * operations);
    CHECK(ctx, sampled && held >= baseline + clients - 2, "did not actually hold 200 simultaneous worker fds (%d -> %d)", baseline, held);
    usleep(250000);
    ASSERT(ctx, sampler.tick(), "quiescent sample failed");
    CHECK(ctx, sampler.fd_last() <= baseline + 2, "worker fd leak after concurrent workload");
    sampler.report("concurrent-fds");
}

TEST("stress.queue_producer_consumer", TAG_STRESS | TAG_QUEUE | TAG_ASAN)
{
    ServerConfig cfg;
    cfg.asan = ctx.expectAsan;
    cfg.threads = 4;
    ServerFixture srv(cfg);
    ASSERT(ctx, srv.start(), "%s", srv.startup_error().c_str());
    constexpr int producers = 4;
    constexpr int consumers = 2;
    const int each = 1000 * ctx.scale;
    const int total = producers * each;
    std::atomic<int> errors{0}, readCount{0}, written{0};
    std::vector<bool> seen(total, false);
    std::mutex seenMutex;
    const uint64_t deadline = now_nanos() + 120000000000ull;
    std::vector<std::thread> pool;
    for (int t = 0; t < producers; ++t) {
        pool.emplace_back([&, t] {
            ScopedConn c(srv);
            if (!c.ok()) { ++errors; return; }
            Rng rng(ctx.seed ^ static_cast<uint64_t>(t + 1));
            for (int i = 0; i < each && !errors.load(); ++i) {
                std::vector<char> record;
                make_payload(record, fx::Q_RECSIZE, t * each + i, rng);
                for (;;) {
                    unsigned err = 0;
                    if (writeq(c.fd(), fx::Q_NORMAL_BIN, record.data(), fx::Q_RECSIZE, &err) && !err) { ++written; break; }
                    if (err != ERROR_DQ_FULL || now_nanos() >= deadline) { ++errors; return; }
                    std::this_thread::yield();
                }
            }
        });
    }
    for (int t = 0; t < consumers; ++t) {
        pool.emplace_back([&] {
            ScopedConn c(srv);
            if (!c.ok()) { ++errors; return; }
            std::vector<char> record(fx::Q_RECSIZE);
            while (readCount < total && !errors.load()) {
                unsigned err = 0;
                if (!readq(c.fd(), fx::Q_NORMAL_BIN, record.data(), fx::Q_RECSIZE, &err)) {
                    if (err != ERROR_DQ_EMPTY || now_nanos() >= deadline) { ++errors; return; }
                    std::this_thread::yield();
                    continue;
                }
                std::string why;
                PayloadTag h{};
                memcpy(&h, record.data(), sizeof(h));
                if (err || !check_payload(record.data(), fx::Q_RECSIZE, -1, &why) || h.seq >= static_cast<unsigned>(total)) {
                    ++errors; return;
                }
                std::lock_guard<std::mutex> lock(seenMutex);
                if (seen[h.seq]) { ++errors; return; }
                seen[h.seq] = true;
                ++readCount;
            }
        });
    }
    for (auto& thread : pool) thread.join();
    CHECK(ctx, errors == 0 && written == total && readCount == total,
          "queue: errors=%d written=%d read=%d expected=%d", errors.load(), written.load(), readCount.load(), total);
    for (int i = 0; i < total; ++i) ASSERT(ctx, seen[i], "lost queue record %d", i);
}
