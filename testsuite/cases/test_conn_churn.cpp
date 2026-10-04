// test_conn_churn.cpp —— 高频短连接：性能 + 资源泄漏（方案 §7.11）
//
// 关注点（与 mem.connpool_hwm_trend 互补：那里特征化“连接对象池按峰值撑大/不缩”的 M1；
//   这里聚焦 **fd 生命周期** 与 **建连性能**，并覆盖异常关闭路径）：
//   [N] qps_latency   —— 大量 connect→readb→disconnect，报告建连延迟分布 + QPS；
//                        **可靠门控**：worker fd 不随 churn 膨胀、quiesce 后回基线（无 fd 泄漏）。
//   [B] rst_abort     —— 用 SO_LINGER{1,0} 让客户端 close 发 RST（而非 FIN），压 recvproc 对
//                        RST/RDHUP/EAGAIN/EINTR 的处理（近期提交刚修过 EAGAIN/EINTR）；
//                        断言服务端存活、仍能正常服务、无 fd 泄漏。
//   [C6][destructive] fdset_overflow —— 单进程把客户端 fd 顶到 ≥1024 后 connectgplat 必须正常建连
//                        （原缺陷 C6 已修复：unblock_connect 以前用 select+FD_SET，fd≥1024 时越界崩溃，
//                        现改用 poll）。fork 隔离，防止回归时拖垮套件。
//
// 隔离纪律：每个用例自建私有 ServerFixture（独立 sandbox + 自动空闲端口，绝不碰 8777）。
#include <fcntl.h>         // open
#include <sys/resource.h> // getrlimit/setrlimit
#include <sys/socket.h>   // setsockopt, SO_LINGER, struct linger
#include <unistd.h>       // close, usleep

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "framework/ts.h"
#include "cases/xfail_ids.h"

using namespace ts;

namespace {

// 私有 server：threads≥2（并发接收），recyWait 可调。
#define PRIVATE_SERVER_T(SRV, NTHREADS, RECY)                                    \
    ServerConfig SRV##_cfg;                                                      \
    SRV##_cfg.threads = (NTHREADS);                                             \
    SRV##_cfg.asan = ctx.expectAsan;                                           \
    SRV##_cfg.recyWait = (RECY);                                                \
    ServerFixture SRV(SRV##_cfg);                                              \
    ASSERT(ctx, SRV.start(), "private fixture start failed: %s", SRV.startup_error().c_str())

constexpr int32_t SENTINEL = (int32_t)0xC04EC04Eu;  // churn 期间每次 readb 校验的数据面哨兵

// 向 fx::TAG_I32 预写哨兵；返回是否成功。
bool write_sentinel(TestContext& ctx, ServerFixture& srv)
{
    ScopedConn c(srv);
    if (!c.ok()) { TS_FAILF(ctx, "sentinel connect failed"); return false; }
    unsigned err = 0;
    int32_t v = SENTINEL;
    if (!writeb(c.fd(), fx::TAG_I32, &v, sizeof(v), &err)) {
        TS_FAILF(ctx, "sentinel writeb failed: err=%u(%s)", err, err_name(err));
        return false;
    }
    return true;
}

}  // namespace

// ===========================================================================
// [N] 建连 QPS / 延迟 + fd 无泄漏门控
// ===========================================================================
static void run_churn(TestContext& ctx, int count)
{
    PRIVATE_SERVER_T(srv, 2, 2);
    pid_t wpid = srv.worker_pid();
    ASSERT(ctx, wpid > 0, "cannot resolve worker pid");
    if (!write_sentinel(ctx, srv)) return;

    const int N     = count * ctx.scale;
    const int CHUNK = 100;               // 每 CHUNK 次采样一次 worker fd

    ProcSampler smp(wpid);
    ASSERT(ctx, smp.tick(), "baseline sample failed");
    const int fd_base = smp.fd_last();

    LatencyHistogram hist;
    hist.reserve(N);

    int ok = 0, connFail = 0;
    const uint64_t t0 = now_nanos();
    for (int i = 0; i < N; i++) {
        const uint64_t cs = now_nanos();
        int fd = srv.open_conn();
        const uint64_t ce = now_nanos();
        if (fd < 0) {
            // 临时端口/backlog 偶发拒绝：计数但不致命（高频 churn 的现实边界）。
            if (++connFail > N / 100 + 8) { TS_FAILF(ctx, "too many connect failures (#%d at i=%d)", connFail, i); break; }
            continue;
        }
        hist.record(ce - cs);
        unsigned err = 0;
        int32_t v = 0;
        if (readb(fd, fx::TAG_I32, &v, sizeof(v), &err, nullptr) && err == 0 && v == SENTINEL)
            ok++;
        else
            TS_FAILF(ctx, "readb #%d r err=%u(%s) v=%08x", i, err, err_name(err), (unsigned)v);
        disconnectgplat(fd);  // 客户端主动 close（FIN）→ 服务端 ngx_close_and_recycle：立即 close(fd)
        if ((i % CHUNK) == CHUNK - 1) smp.tick();
    }
    const uint64_t t1 = now_nanos();
    smp.tick();
    const int fd_peak = smp.fd_max();

    // quiesce：停 churn，等 fd 回收（fd 立即关闭，与 recyWait 无关，但给点余量）。
    usleep(1500 * 1000);
    ASSERT(ctx, smp.tick(), "post-quiesce sample failed");
    const int fd_quiesce = smp.fd_last();

    const double secs = (t1 - t0) / 1e9;
    printf("    [perf] %d conns in %.3fs → %.0f conn/s；建连延迟："
           "p50=%.1f p99=%.1f p999=%.1f max=%.1f us（connFail=%d）\n",
           ok, secs, ok / (secs > 0 ? secs : 1), hist.percentile_us(50), hist.percentile_us(99),
           hist.percentile_us(99.9), hist.max_us(), connFail);
    printf("    [note] worker fd: base=%d peak=%d quiesce=%d\n", fd_base, fd_peak, fd_quiesce);

    // 正确性门控：全部读回哨兵正确。
    CHECK(ctx, ok + connFail == N, "accounted %d != N %d", ok + connFail, N);
    CHECK(ctx, connFail <= N / 100 + 8, "excessive connect failures: %d", connFail);
    // fd 门控【稳健回归闸门】：顺序短连接至多 1 条在途 → worker fd 几乎不动；quiesce 回基线。
    CHECK(ctx, fd_peak <= fd_base + 8,
          "worker fd grew during churn: peak %d vs base %d (fd 应随 disconnect 立即关闭)", fd_peak, fd_base);
    CHECK(ctx, fd_quiesce <= fd_base + 4,
          "worker fd not reclaimed: quiesce %d vs base %d (疑似 fd 泄漏)", fd_quiesce, fd_base);
}

TEST("conn_churn.qps_latency", TAG_CONN) { run_churn(ctx, 3000); }
TEST("conn_churn.extreme_short_connections", TAG_CONN | TAG_STRESS) { run_churn(ctx, 50000); }

// ===========================================================================
// [B] RST 关闭（SO_LINGER{1,0}）压 recvproc 异常路径 —— 服务端须存活、仍能服务、无 fd 泄漏
// ===========================================================================
TEST("conn_churn.rst_abort", TAG_CONN)
{
    PRIVATE_SERVER_T(srv, 2, 2);
    pid_t wpid = srv.worker_pid();
    ASSERT(ctx, wpid > 0, "cannot resolve worker pid");
    if (!write_sentinel(ctx, srv)) return;

    ProcSampler smp(wpid);
    ASSERT(ctx, smp.tick(), "baseline sample failed");
    const int fd_base = smp.fd_last();

    const int N = 2000 * ctx.scale;
    int rst = 0, served = 0;
    Rng rng(ctx.seed);

    for (int i = 0; i < N; i++) {
        int fd = srv.open_conn();
        ASSERT(ctx, fd >= 0, "RST storm connect failed at %d", i);

        // 一半连接先发一次正常 readb（制造“请求处理中/已回包”的在途状态），再 RST；
        // 另一半连上立刻 RST（连接刚建立就被 abort，压 accept 后的首个事件处理）。
        if (rng.chance(50)) {
            unsigned err = 0;
            int32_t v = 0;
            ASSERT(ctx, readb(fd, fx::TAG_I32, &v, sizeof(v), &err, nullptr) &&
                   err == 0 && v == SENTINEL, "RST pre-read failed at %d (err=%u)", i, err);
            served++;
        }
        // SO_LINGER{on=1, linger=0} → close 立即发 RST（跳过 FIN/TIME_WAIT），
        // 服务端 recv 侧会读到 ECONNRESET/EPOLLERR/RDHUP，走异常关闭分支。
        struct linger lg{};
        lg.l_onoff = 1;
        lg.l_linger = 0;
        ASSERT(ctx, setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg)) == 0, "SO_LINGER failed");
        close(fd);  // 直接 close 以确保 RST（不走 disconnectgplat 的常规 close）
        rst++;

        if ((i % 200) == 199) {
            smp.tick();
            // 过程中周期性确认服务端仍在处理请求（未被异常关闭打崩/卡死）。
            if (!srv.running()) { TS_FAILF(ctx, "server died during RST storm at i=%d", i); break; }
        }
    }

    printf("    [info] RST storm: %d resets (%d 先读后 RST), server alive=%d\n", rst, served, (int)srv.running());
    ASSERT(ctx, srv.running(), "server not alive after RST storm");

    // 存活性 + 功能性：RST 风暴后仍能建立正常连接并读到正确哨兵。
    {
        ScopedConn c(srv);
        ASSERT(ctx, c.ok(), "post-RST clean connect failed");
        unsigned err = 0;
        int32_t v = 0;
        EXPECT_OK(ctx, readb(c.fd(), fx::TAG_I32, &v, sizeof(v), &err, nullptr), err);
        CHECK(ctx, v == SENTINEL, "post-RST sentinel mismatch: %08x", (unsigned)v);
    }

    // fd 无泄漏：RST 的连接服务端也应及时 close(fd)。
    usleep(1500 * 1000);
    ASSERT(ctx, smp.tick(), "post-quiesce sample failed");
    const int fd_quiesce = smp.fd_last();
    printf("    [note] worker fd: base=%d quiesce=%d\n", fd_base, fd_quiesce);
    CHECK(ctx, fd_quiesce <= fd_base + 4,
          "worker fd not reclaimed after RST storm: %d vs base %d (RST 路径 fd 泄漏?)", fd_quiesce, fd_base);
}

// ===========================================================================
// [C6][destructive] 客户端 fd≥1024 时 connectgplat 仍应正常建连（原 FD_SET 栈溢出已修复）
// ===========================================================================
// 原机理：connectgplat 非阻塞 connect 后用 select + FD_SET 等可写，fd_set 只有 FD_SETSIZE(1024) 位，
//   sockfd≥1024 时越界写栈 → 崩溃。现 unblock_connect 改用 poll，无 fd 上限。
// 用 fork 隔离：子进程先抬高 RLIMIT_NOFILE、打开占位 fd 把下一个 fd 顶到 ≥1024，再 connectgplat。
TEST("conn_churn.fdset_overflow", TAG_CONN | TAG_DESTRUCTIVE)
{
    if (!ctx.expectAsan) {
        ctx.skip("C6 requires ASan to detect an out-of-bounds FD_SET regression");
        return;
    }
    // 可行性前置：需要能打开 fd≥1024。硬上限过低则无法覆盖 → skip。
    struct rlimit rl{};
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0 || rl.rlim_max < 1100) {
        ctx.skip("RLIMIT_NOFILE hard limit < 1100, cannot reach fd>=1024 to trigger C6");
        return;
    }

    PRIVATE_SERVER_T(srv, 2, 2);
    const std::string ip = srv.ip();
    const int port = srv.port();

    // 子进程体：顶高 fd 后 connectgplat —— 若触发 FD_SET 越界则崩溃。
    ForkResult r = run_in_fork([&]() {
        struct rlimit want{};
        want.rlim_cur = (rl.rlim_max < 4096 ? rl.rlim_max : 4096);
        want.rlim_max = rl.rlim_max;
        if (setrlimit(RLIMIT_NOFILE, &want) != 0) _exit(77);

        // 打开占位 fd，直到最近一个 fd >= 1100（把 connectgplat 的 socket fd 顶到 ≥1024，
        // 且越界写落点远离 fd_set 末端，提高崩溃确定性）。
        int last = -1;
        for (int guard = 0; guard < 4000 && last < 1100; guard++) {
            int d = open("/dev/null", O_RDONLY);
            if (d < 0) break;
            last = d;
        }
        if (last < 1100) _exit(77);
        // 此刻下一个 socket() 将返回 >=1101 的 fd。connectgplat 必须在高 fd 下正常建连。
        int fd = connectgplat(ip.c_str(), port);
        if (fd < 0) _exit(2);
        disconnectgplat(fd);
        _exit(0);
    }, 8000);

    printf("    [info] fork result: %s\n", r.describe());

    ASSERT(ctx, !r.timed_out && !(r.exited && r.exit_code == 77), "C6 setup/watchdog failed: %s", r.describe());
    CHECK(ctx, r.exited && r.exit_code == 0,
          "C6 regression: connectgplat failed/crashed with fd>=1024 (%s)\n%s", r.describe(), r.diagnostics.c_str());
}
