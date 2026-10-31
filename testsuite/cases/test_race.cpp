// test_race.cpp —— 线程交错：断连清理 × 在途请求、等待超时定时器的取消与复用（破坏性）
//
// 这些缺陷靠常规用例很难撞到，这里用“裸协议流水线 + 立即半关闭”把窗口放大：
//   [N] postwait_superseded_timer —— 同一连接上一次 POSTWAIT 的超时定时器被后一次 POSTWAIT 取代后，
//                                   不得在之后的某次等待里“提前”触发 WAIT_TIMEOUT（定时器身份必须可验证）。
//   [N] postwait_close_no_crosstalk —— 流水线 POSTWAIT 后立即断开：在途的 POSTWAIT 若晚于断连清理执行，
//                                   不得在已关闭的连接上重新进入等待并启动定时器；否则定时器到期时
//                                   会把 WAIT_TIMEOUT 按“当前序号”发到已关闭、可能已被新连接复用的 fd 上，
//                                   新连接收到不请自来的数据（串扰）。
//   [N] subscribe_close_no_stale —— 流水线 SUBSCRIBE / REGISTERPLCSERVER 后立即断开：在途请求若晚于清理执行，
//                                   不得在已断开的连接对象上留下订阅/PLC 注册；否则该对象被新客户端复用后，
//                                   新客户端会收到它从未订阅过的事件。
//   [N] shutdown_with_busy_timers —— 停机时仍有大量延时投递/定时器订阅回调陆续到期：worker 释放连接池之前
//                                   必须先停定时器线程，否则回调会锁已释放连接对象里的互斥量（ASan 下为 use-after-free）。
//   [N] post_order_single_publisher —— 同一发布者顺序写同一 tag（writeb / write_plc_int，每次等应答）：
//                                   订阅者 / PLC 服务器收到的事件必须保持写入顺序。多个消息线程并发处理时，
//                                   若先应答发布者、后投递事件，下一次写入的事件可能抢在前面（PLC 最终停在旧值）。
//
// 隔离纪律：每个用例自建私有 ServerFixture（独立 sandbox + 自动空闲端口）。
// 破坏性门控：标 TAG_DESTRUCTIVE，默认隐藏，`make test-destructive` / `make test-all` 运行。
#include <dirent.h>
#include <poll.h>
#include <sched.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "framework/ts.h"

using namespace ts;

namespace {

// 私有 server：多个消息线程（在途请求才会并发），连接池很小、回收很快，便于复用连接对象。
#define PRIVATE_RACE_SERVER(SRV, CONNS, RECY)                                   \
    ServerConfig SRV##_cfg;                                                     \
    SRV##_cfg.threads = 3;                                                      \
    SRV##_cfg.workerConns = (CONNS);                                            \
    SRV##_cfg.recyWait = (RECY);                                                \
    SRV##_cfg.asan = ctx.expectAsan;                                            \
    ServerFixture SRV(SRV##_cfg);                                               \
    ASSERT(ctx, SRV.start(), "private race fixture start failed: %s", SRV.startup_error().c_str())

double elapsed_ms(uint64_t t0)
{
    return (double)(now_nanos() - t0) / 1e6;
}

// 把 count 个相同的包头一次性发出（流水线），然后半关闭写端：服务端读完这些请求后立即看到 EOF。
bool pipeline_and_half_close(int fd, const std::vector<MSGHEAD>& heads)
{
    std::vector<char> buf(heads.size() * sizeof(MSGHEAD));
    memcpy(buf.data(), heads.data(), buf.size());
    if (!raw_send(fd, buf.data(), buf.size()))
        return false;
    return shutdown(fd, SHUT_WR) == 0;
}

// fd 上在 waitMs 内是否有任何可读数据（不请自来的数据）
bool has_unsolicited_data(int fd, int waitMs)
{
    pollfd p{fd, POLLIN, 0};
    if (poll(&p, 1, waitMs) <= 0)
        return false;
    char c;
    return recv(fd, &c, 1, MSG_DONTWAIT | MSG_PEEK) == 1;
}

}  // namespace

// ===========================================================================
// [N] 被取代的等待超时定时器不得在之后的等待里提前触发
// ===========================================================================
TEST("race.postwait_superseded_timer", TAG_PUBSUB | TAG_DESTRUCTIVE)
{
    PRIVATE_RACE_SERVER(srv, 64, 20);

    int fd = raw_connect(srv.ip(), srv.port(), 6000);
    ASSERT(ctx, fd >= 0, "raw connect failed");

    // 第一次等待 1500ms；50ms 后第二次等待 100ms 取代它（裸协议可以流水线发 POSTWAIT）。
    MSGHEAD w = make_head(POSTWAIT, "BOARD", "");
    w.bodysize = 0;
    w.timeout = 1500;
    ASSERT(ctx, send_msg(fd, w), "send POSTWAIT(1500) failed");
    usleep(50 * 1000);
    w.timeout = 100;
    ASSERT(ctx, send_msg(fd, w), "send POSTWAIT(100) failed");

    MSGHEAD out{};
    std::vector<char> body;
    ASSERT(ctx, recv_msg(fd, out, body) == RecvStatus::OK && out.id == POSTWAIT && out.error == ERROR_WAIT_TIMEOUT,
           "expected one WAIT_TIMEOUT for the superseded waits (id=%d err=%u)", out.id, out.error);

    // 新的一次 3000ms 等待：1500ms 的旧定时器若仍有效，会在约 1450ms 时把它提前结束。
    w.timeout = 3000;
    uint64_t t0 = now_nanos();
    ASSERT(ctx, send_msg(fd, w), "send POSTWAIT(3000) failed");
    out = MSGHEAD{};
    ASSERT(ctx, recv_msg(fd, out, body) == RecvStatus::OK && out.id == POSTWAIT && out.error == ERROR_WAIT_TIMEOUT,
           "expected WAIT_TIMEOUT for the 3000ms wait (id=%d err=%u)", out.id, out.error);
    double ms = elapsed_ms(t0);
    CHECK(ctx, ms >= 2500.0, "3000ms wait ended after %.0f ms: a superseded wait timer fired into it", ms);

    // 期间不应再有多余的 WAIT_TIMEOUT
    CHECK(ctx, !has_unsolicited_data(fd, 300), "extra packet after the 3000ms wait");
    close(fd);
    CHECK(ctx, srv.running(), "server died");
}

// ===========================================================================
// [N] 在途 POSTWAIT 晚于断连清理：不得把 WAIT_TIMEOUT 发到被新连接复用的 fd 上
// ===========================================================================
TEST("race.postwait_close_no_crosstalk", TAG_PUBSUB | TAG_CONN | TAG_DESTRUCTIVE)
{
    PRIVATE_RACE_SERVER(srv, 64, 20);

    constexpr int kRounds = 8;
    constexpr int kPairs = 8;
    constexpr int kPipelined = 200;
    constexpr int kWaitMs = 250;

    MSGHEAD w = make_head(POSTWAIT, "BOARD", "");
    w.bodysize = 0;
    w.timeout = kWaitMs;
    const std::vector<MSGHEAD> heads(kPipelined, w);

    int crosstalk = 0;
    for (int round = 0; round < kRounds; round++) {
        int a[kPairs];
        for (int i = 0; i < kPairs; i++) {
            a[i] = raw_connect(srv.ip(), srv.port(), 1000);
            ASSERT(ctx, a[i] >= 0, "round %d: connect A%d failed", round, i);
        }
        for (int i = 0; i < kPairs; i++)
            ASSERT(ctx, pipeline_and_half_close(a[i], heads), "round %d: pipeline A%d failed", round, i);

        usleep(30 * 1000);  // 服务端读完请求、看到 EOF 并关闭这些连接
        for (int i = 0; i < kPairs; i++)
            close(a[i]);

        // 新连接复用刚释放的 fd 号；它们什么都没发，因此不应收到任何数据
        int b[kPairs];
        for (int i = 0; i < kPairs; i++) {
            b[i] = raw_connect(srv.ip(), srv.port(), 1000);
            ASSERT(ctx, b[i] >= 0, "round %d: connect B%d failed", round, i);
        }
        uint64_t t0 = now_nanos();
        for (int i = 0; i < kPairs; i++) {
            int left = 3 * kWaitMs - (int)elapsed_ms(t0);
            if (has_unsolicited_data(b[i], left > 0 ? left : 0)) {
                crosstalk++;
                printf("    [info] round %d: B%d received unsolicited data\n", round, i);
            }
        }
        for (int i = 0; i < kPairs; i++)
            close(b[i]);
    }

    CHECK(ctx, crosstalk == 0, "%d fresh connections received another connection's WAIT_TIMEOUT", crosstalk);
    CHECK(ctx, srv.running(), "server died");
}

// ===========================================================================
// [N] 在途 SUBSCRIBE / REGISTERPLCSERVER 晚于断连清理：连接对象复用后不得收到别人的事件
// ===========================================================================
TEST("race.subscribe_close_no_stale", TAG_PUBSUB | TAG_CONN | TAG_DESTRUCTIVE)
{
    // 连接池只有 4 个对象、回收等待 1s：断开的连接对象很快回到空闲表并被后来的连接复用
    PRIVATE_RACE_SERVER(srv, 4, 1);

    constexpr int kVictims = 24;
    constexpr int kPipelined = 300;
    const char* kPlcTag = "RACE_PLC_TAG";

    std::vector<MSGHEAD> heads;
    for (int i = 0; i < kPipelined; i++) {
        MSGHEAD h{};
        switch (i % 3) {
        case 0:
            h = make_head(SUBSCRIBE, fx::TAG_POST, fx::TAG_POST);
            h.eventid = 1;  // DEFAULT
            break;
        case 1:
            h = make_head(SUBSCRIBE, fx::TAG_I32, fx::TAG_I32);
            h.eventid = 1;
            break;
        default:
            h = make_head(REGISTERPLCSERVER, kPlcTag, kPlcTag);
            break;
        }
        h.bodysize = 0;
        heads.push_back(h);
    }

    for (int v = 0; v < kVictims; v++) {
        int fd = raw_connect(srv.ip(), srv.port(), 1000);
        ASSERT(ctx, fd >= 0, "connect victim %d failed", v);
        ASSERT(ctx, pipeline_and_half_close(fd, heads), "pipeline victim %d failed", v);
        usleep(20 * 1000);
        close(fd);
    }

    // 等回收线程把所有断开的连接对象还回空闲表（recyWait=1s + 200ms 扫描粒度 + 余量）
    usleep(2000 * 1000);

    // 足够多的新连接，覆盖整个连接池；它们都不订阅、不注册
    constexpr int kProbes = kVictims + 8;
    std::vector<int> probes;
    for (int i = 0; i < kProbes; i++) {
        int fd = connectgplat(srv.ip(), srv.port());
        ASSERT(ctx, fd >= 0, "connect probe %d failed", i);
        probes.push_back(fd);
    }

    ScopedConn pub(srv);
    ASSERT(ctx, pub.ok(), "connect publisher failed");
    unsigned err = 0;
    std::vector<char> post(256, 'x');
    REQUIRE_OK(ctx, writeb(pub.fd(), fx::TAG_POST, post.data(), (int)post.size(), &err), err);
    int32_t i32 = 42;
    REQUIRE_OK(ctx, writeb(pub.fd(), fx::TAG_I32, &i32, sizeof(i32), &err), err);
    write_plc_int(pub.fd(), kPlcTag, 7, &err);  // 没有存活的 PLC 服务器：只会记一条日志
    usleep(200 * 1000);

    int stale = 0;
    for (size_t i = 0; i < probes.size(); i++) {
        char tag[64] = {0};
        char val[512] = {0};
        unsigned werr = 0;
        bool ok = waitpostdata(probes[i], tag, sizeof(tag), val, sizeof(val), 0, &werr);
        if (ok && werr == 0) {
            stale++;
            printf("    [info] probe %zu received an event for '%s' it never subscribed to\n", i, tag);
        }
    }
    for (int fd : probes)
        disconnectgplat(fd);

    CHECK(ctx, stale == 0, "%d reused connections received events from a dead connection's subscription", stale);
    CHECK(ctx, srv.running(), "server died");
}

// ===========================================================================
// [N] 停机时定时器回调仍密集到期：不得访问已释放的连接对象
// ===========================================================================
TEST("race.shutdown_with_busy_timers", TAG_PUBSUB | TAG_DESTRUCTIVE | TAG_ASAN)
{
    ServerConfig cfg;
    cfg.threads = 3;
    cfg.asan = ctx.expectAsan;
    cfg.allowLeaks = true;  // 停机时仍连着的客户端、尚未到期的延时投递会留下已知的 shutdown-leak（见 mem.live_conn_shutdown_leak）
    ServerFixture srv(cfg);
    ASSERT(ctx, srv.start(), "fixture start failed: %s", srv.startup_error().c_str());

    // 几个订阅方：各自对 TAG_POST 做不同延时的 POST_DELAY 订阅，并订阅 timer_500ms。
    // 每次写 TAG_POST 都会为每个延时订阅排一个定时器，停机时堆里有大量在之后几百毫秒内陆续到期的回调。
    constexpr int kSubscribers = 4;
    std::vector<int> subs;
    unsigned err = 0;
    for (int i = 0; i < kSubscribers; i++) {
        int fd = srv.open_conn();
        ASSERT(ctx, fd >= 0, "connect subscriber %d failed", i);
        subs.push_back(fd);
        char evt[32];
        snprintf(evt, sizeof(evt), "EVT_SHUTDOWN_%d", i);
        REQUIRE_OK(ctx, subscribedelaypost(fd, fx::TAG_POST, evt, 100 + 100 * i, &err), err);
        REQUIRE_OK(ctx, subscribe(fd, "timer_500ms", &err), err);
    }

    std::atomic<bool> stop{false};
    std::atomic<long> writes{0};
    const int wfd = srv.open_conn();
    ASSERT(ctx, wfd >= 0, "connect writer failed");
    std::thread writer([&] {
        std::vector<char> post(256, 'w');
        unsigned werr = 0;
        while (!stop.load()) {
            if (!writeb(wfd, fx::TAG_POST, post.data(), (int)post.size(), &werr))
                break;  // 服务端已退出
            ++writes;
        }
    });

    usleep(600 * 1000);
    srv.stop();  // SIGTERM：此刻定时器堆里还有大量尚未到期的延时投递
    stop = true;
    writer.join();
    disconnectgplat(wfd);
    for (int fd : subs)
        disconnectgplat(fd);

    printf("    [info] %ld writes before shutdown\n", writes.load());
    CHECK(ctx, writes.load() > 100, "writer barely ran (%ld writes): timers were not exercised", writes.load());
    CHECK(ctx, !srv.running(), "server still running after stop()");
    // ASan 构建下 fixture 析构时检查 asan.* 报告：任何内存错误（heap-use-after-free 等）都会记为失败
}

// ===========================================================================
// [N] 同一发布者的顺序写：订阅者和 PLC 服务器收到的事件不得乱序
// ===========================================================================
namespace {

// 在 fd 上持续 waitpostdata，把每个事件载荷前 4 字节（序号）依次记下，直到收满 want 个或超时
void collect_seqs(int fd, int want, std::vector<int32_t>& out)
{
    char tag[64];
    char val[512];
    while ((int)out.size() < want) {
        unsigned err = 0;
        if (!waitpostdata(fd, tag, sizeof(tag), val, sizeof(val), 2000, &err) || err != 0)
            return;  // 超时或出错：收到多少算多少
        int32_t seq = 0;
        memcpy(&seq, val, sizeof(seq));
        out.push_back(seq);
    }
}

// 把进程的所有线程钉到同一个 CPU：线程间频繁抢占，放大“应答之后、投递之前”被抢占的窗口
bool pin_all_threads(pid_t pid, int cpu)
{
    std::string dir = "/proc/" + std::to_string(pid) + "/task";
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    int pinned = 0;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        if (sched_setaffinity((pid_t)atoi(e->d_name), sizeof(set), &set) == 0) pinned++;
    }
    closedir(d);
    return pinned > 0;
}

int count_inversions(const std::vector<int32_t>& seqs)
{
    int inv = 0;
    for (size_t i = 1; i < seqs.size(); i++)
        if (seqs[i] <= seqs[i - 1]) inv++;
    return inv;
}

}  // namespace

TEST("race.post_order_single_publisher", TAG_PUBSUB | TAG_DESTRUCTIVE)
{
    ServerConfig cfg;
    cfg.threads = 4;  // 多个消息线程：同一发布者的相邻两次写可能由不同线程处理
    cfg.asan = ctx.expectAsan;
    ServerFixture srv(cfg);
    ASSERT(ctx, srv.start(), "fixture start failed: %s", srv.startup_error().c_str());

    constexpr int kWrites = 3000;
    const char* kPlcTag = "ORDER_PLC_TAG";
    unsigned err = 0;

    // 先订阅的一批“压舱”订阅者（从不取事件）：每次投递要先依次经过它们，
    // 拉长“应答发布者”到“投递到最后一个订阅者”的时间，这是真实系统里一个 tag 有很多订阅者时的样子
    constexpr int kBallast = 100;
    std::vector<ScopedConn> ballast;
    for (int i = 0; i < kBallast; i++) {
        ballast.emplace_back(srv);
        ASSERT(ctx, ballast.back().ok(), "connect ballast %d failed", i);
        REQUIRE_OK(ctx, subscribe(ballast.back().fd(), fx::TAG_POST, &err), err);
    }

    ScopedConn sub(srv), plc(srv), pub(srv);
    ASSERT(ctx, sub.ok() && plc.ok() && pub.ok(), "connect failed");
    REQUIRE_OK(ctx, subscribe(sub.fd(), fx::TAG_POST, &err), err);
    REQUIRE_OK(ctx, registertag(plc.fd(), kPlcTag, &err), err);

    // worker 的全部线程挤在一个 CPU 上，再加一个同 CPU 的忙等线程制造抢占
    const pid_t wpid = srv.worker_pid();
    ASSERT(ctx, wpid > 0 && pin_all_threads(wpid, 0), "cannot pin worker threads");
    std::atomic<bool> burn{true};
    std::thread burner([&] {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(0, &set);
        sched_setaffinity(0, sizeof(set), &set);
        while (burn.load(std::memory_order_relaxed)) {}
    });

    std::vector<int32_t> subSeqs, plcSeqs;
    std::thread subThread([&] { collect_seqs(sub.fd(), kWrites, subSeqs); });
    std::thread plcThread([&] { collect_seqs(plc.fd(), kWrites, plcSeqs); });

    std::vector<char> post(256, 0);
    for (int32_t i = 0; i < kWrites; i++) {
        memcpy(post.data(), &i, sizeof(i));
        unsigned werr = 0;
        if (!writeb(pub.fd(), fx::TAG_POST, post.data(), (int)post.size(), &werr) ||
            !write_plc_int(pub.fd(), kPlcTag, i, &werr)) {
            TS_FAILF(ctx, "write #%d failed: err=%u(%s)", i, werr, err_name(werr));
            break;
        }
    }
    subThread.join();
    plcThread.join();
    burn = false;
    burner.join();

    const int subInv = count_inversions(subSeqs);
    const int plcInv = count_inversions(plcSeqs);
    printf("    [info] subscriber got %zu events (%d out of order), PLC server got %zu events (%d out of order)\n",
           subSeqs.size(), subInv, plcSeqs.size(), plcInv);
    CHECK(ctx, subInv == 0, "subscriber received %d events out of write order", subInv);
    CHECK(ctx, plcInv == 0, "PLC server received %d writes out of order", plcInv);
    CHECK(ctx, !plcSeqs.empty() && plcSeqs.back() == kWrites - 1,
          "PLC server's last write is %d, want %d", plcSeqs.empty() ? -1 : plcSeqs.back(), kWrites - 1);
}
