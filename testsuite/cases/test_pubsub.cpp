// test_pubsub.cpp —— 发布/订阅：subscribe / waitpostdata / subscribedelaypost（方案 §7.6 非破坏象限）
//
// 投递语义（已对照 gplat/ngx_c_slogic.cxx 源码确认）：
//   * subscribe(tag)       → 以 DEFAULT 事件挂到 tag 上；另一连接 writeb(tag) 触发投递，
//                            waitpostdata 收到 tagname==被写 tag、value==该 tag 全量数据。
//   * writeb_notpost(tag)  → 不触发投递。
//   * subscribedelaypost(tag,eventname,ms) → POST_DELAY 事件；writeb(tag) 触发后延迟 ms 投递，
//                            waitpostdata 收到 tagname==eventname（不是 tag！）、value==数据。
//   * 投递先入连接的 m_listPost 队列；waitpostdata 非空即取队首，空则按 timeout 等待。
//   * waitpostdata 超时：返回 true、error==WAIT_TIMEOUT、tagname=="WAIT_TIMEOUT"（注意返回 true）。
//   * 非法入参（tagnamesize<40 / buffersize<=0 / timeout<-1）：返回 false、INVALID_PARAMETER，且不关 socket。
//
// 跨连接前提：sandbox 配置 WorkerProcesses=1（单 worker 进程），m_subscriber 进程内共享。
// 共享 server 纪律：订阅随连接断开被 CancelSubscribe 清理（见 ngx_c_socket_request.cxx:44），
//   故每个用例用全新 ScopedConn 订阅、退出即净；只 writeb 夹具 tag TAG_POST，不碰其它夹具。
#include <cstring>
#include <string>
#include <vector>

#include "framework/ts.h"
#include "cases/xfail_ids.h"

using namespace ts;

namespace {

constexpr int POST_SZ = 256;   // TAG_POST 的 itemsize（char[256]）；writeb 必须恰好 256B
constexpr int TAGBUF  = 64;    // waitpostdata tagname 缓冲（≥GPLAT_TAGNAME_SIZE=40）
constexpr int VALBUF  = 512;   // 收包缓冲（≥POST_SZ，留 NUL 余量）

// 从另一连接向 TAG_POST 写一条带自校验头的 256B 负载；post=true 用 writeb（触发），false 用 writeb_notpost。
void publish(TestContext& ctx, int fd, uint32_t seq, Rng& rng, bool post)
{
    unsigned err = 0;
    std::vector<char> payload;
    make_payload(payload, POST_SZ, seq, rng);
    if (post)
        REQUIRE_OK(ctx, writeb(fd, fx::TAG_POST, payload.data(), POST_SZ, &err), err);
    else
        REQUIRE_OK(ctx, writeb_notpost(fd, fx::TAG_POST, payload.data(), POST_SZ, &err), err);
}

// 一次投递的接收结果。
struct Recv {
    bool     ok = false;
    unsigned err = 0;
    char     tag[TAGBUF] = {0};
    std::vector<char> val;
    double   ms = 0.0;
};

// 封装 waitpostdata：计时 + 填好缓冲。
Recv receive(int fd, int timeout_ms)
{
    Recv r;
    r.val.assign(VALBUF, 0);
    uint64_t t0 = now_nanos();
    r.ok = waitpostdata(fd, r.tag, TAGBUF, r.val.data(), VALBUF, timeout_ms, &r.err);
    r.ms = (double)(now_nanos() - t0) / 1e6;
    return r;
}

}  // namespace

// --- [N] 基本投递：subscribe → 他连接 writeb → waitpostdata 收到 tag+数据 ----
TEST("pubsub.basic_post", TAG_PUBSUB)
{
    ScopedConn sub(ctx.server()), pub(ctx.server());
    ASSERT(ctx, sub.ok() && pub.ok(), "connect failed");
    unsigned err = 0;
    Rng rng(ctx.seed);

    REQUIRE_OK(ctx, subscribe(sub.fd(), fx::TAG_POST, &err), err);
    publish(ctx, pub.fd(), 1, rng, /*post=*/true);  // writeb → 入 sub 的 m_listPost

    Recv r = receive(sub.fd(), 2000);
    ASSERT(ctx, r.ok && r.err == 0, "waitpostdata: ret=%d err=%u(%s)", (int)r.ok, r.err, err_name(r.err));
    CHECK(ctx, strcmp(r.tag, fx::TAG_POST) == 0, "delivered tagname='%s' want '%s'", r.tag, fx::TAG_POST);
    std::string perr;
    CHECK(ctx, check_payload(r.val.data(), POST_SZ, 1, &perr), "delivered payload: %s", perr.c_str());
}

// --- [N] writeb_notpost 不触发：订阅后 notpost 写入 → waitpostdata 超时 ------
TEST("pubsub.notpost_no_trigger", TAG_PUBSUB)
{
    ScopedConn sub(ctx.server()), pub(ctx.server());
    ASSERT(ctx, sub.ok() && pub.ok(), "connect failed");
    unsigned err = 0;
    Rng rng(ctx.seed);

    REQUIRE_OK(ctx, subscribe(sub.fd(), fx::TAG_POST, &err), err);
    publish(ctx, pub.fd(), 7, rng, /*post=*/false);  // writeb_notpost → 不应投递

    Recv r = receive(sub.fd(), 300);  // 无投递 → 预期超时
    CHECK(ctx, r.ok && r.err == ERROR_WAIT_TIMEOUT, "notpost should time out: ret=%d err=%u(%s)",
          (int)r.ok, r.err, err_name(r.err));
    CHECK(ctx, strcmp(r.tag, "WAIT_TIMEOUT") == 0, "timeout tagname='%s' want WAIT_TIMEOUT", r.tag);
}

// --- [B] 挂起队列：waitpostdata 前先连写 3 条，之后 FIFO 逐条取出 -----------
TEST("pubsub.pending_queue_fifo", TAG_PUBSUB)
{
    ScopedConn sub(ctx.server()), pub(ctx.server());
    ASSERT(ctx, sub.ok() && pub.ok(), "connect failed");
    unsigned err = 0;
    Rng rng(ctx.seed);

    REQUIRE_OK(ctx, subscribe(sub.fd(), fx::TAG_POST, &err), err);
    // 在任何 waitpostdata 之前连写 3 条：均入 m_listPost 排队（m_bWaitingPost=false 路径）。
    for (uint32_t i = 0; i < 3; i++) publish(ctx, pub.fd(), i, rng, /*post=*/true);

    // 逐条取出，校验 FIFO 顺序（0,1,2）。注意：同一 tag 多次写，载荷随机但 seq 递增可辨。
    // 载荷用独立 Rng 复算以比对内容。
    Rng chk(ctx.seed);
    for (uint32_t i = 0; i < 3; i++) {
        Recv r = receive(sub.fd(), 2000);
        ASSERT(ctx, r.ok && r.err == 0, "drain %u: ret=%d err=%u(%s)", i, (int)r.ok, r.err, err_name(r.err));
        CHECK(ctx, strcmp(r.tag, fx::TAG_POST) == 0, "drain %u tagname='%s'", i, r.tag);
        std::vector<char> expect;
        make_payload(expect, POST_SZ, i, chk);  // 与 publish 同序列、同 seq → 内容一致
        CHECK(ctx, memcmp(r.val.data(), expect.data(), POST_SZ) == 0, "drain %u payload mismatch", i);
    }
}

// --- [B] 超时 timeout=0：立即返回 WAIT_TIMEOUT -----------------------------
TEST("pubsub.wait_timeout_zero", TAG_PUBSUB)
{
    ScopedConn sub(ctx.server());
    ASSERT(ctx, sub.ok(), "connect failed");
    unsigned err = 0;
    REQUIRE_OK(ctx, subscribe(sub.fd(), fx::TAG_POST, &err), err);

    Recv r = receive(sub.fd(), 0);  // 无投递 + timeout 0 → 立即超时
    CHECK(ctx, r.ok && r.err == ERROR_WAIT_TIMEOUT, "timeout0: ret=%d err=%u(%s)",
          (int)r.ok, r.err, err_name(r.err));
    CHECK(ctx, strcmp(r.tag, "WAIT_TIMEOUT") == 0, "timeout0 tagname='%s'", r.tag);
    CHECK(ctx, r.ms < 500.0, "timeout0 should be immediate, took %.1f ms", r.ms);
}

// --- [B] 超时 timeout=300ms：约定时返回 WAIT_TIMEOUT -----------------------
TEST("pubsub.wait_timeout_timed", TAG_PUBSUB)
{
    ScopedConn sub(ctx.server());
    ASSERT(ctx, sub.ok(), "connect failed");
    unsigned err = 0;
    REQUIRE_OK(ctx, subscribe(sub.fd(), fx::TAG_POST, &err), err);

    Recv r = receive(sub.fd(), 300);
    CHECK(ctx, r.ok && r.err == ERROR_WAIT_TIMEOUT, "timed: ret=%d err=%u(%s)",
          (int)r.ok, r.err, err_name(r.err));
    // 下界校验“确实等了”；上界给足余量避免偶发抖动误报。
    CHECK(ctx, r.ms >= 200.0 && r.ms < 3000.0, "timed wait took %.1f ms (want ~300)", r.ms);
}

// --- [I] 非法入参 → INVALID_PARAMETER（且不关 socket，可连用） --------------
TEST("pubsub.invalid_params", TAG_PUBSUB)
{
    ScopedConn sub(ctx.server());
    ASSERT(ctx, sub.ok(), "connect failed");
    int fd = sub.fd();
    unsigned err = 0;
    char tag[TAGBUF] = {0};
    std::vector<char> val(VALBUF, 0);

    // tagnamesize < 40
    EXPECT_ERR(ctx, waitpostdata(fd, tag, 8, val.data(), VALBUF, 0, &err), err, ERROR_INVALID_PARAMETER);
    // buffersize <= 0
    EXPECT_ERR(ctx, waitpostdata(fd, tag, TAGBUF, val.data(), 0, 0, &err), err, ERROR_INVALID_PARAMETER);
    // timeout < -1
    EXPECT_ERR(ctx, waitpostdata(fd, tag, TAGBUF, val.data(), VALBUF, -2, &err), err, ERROR_INVALID_PARAMETER);
    // 连接应仍可用（上面不该关 socket）：一次正常 timeout=0 应成功返回超时。
    Recv r = receive(fd, 0);
    CHECK(ctx, r.ok && r.err == ERROR_WAIT_TIMEOUT, "conn still usable after invalid params: err=%u(%s)",
          r.err, err_name(r.err));
}

// --- [N] 延迟投递 subscribedelaypost：延迟后投递、tagname==eventname --------
TEST("pubsub.delaypost", TAG_PUBSUB)
{
    ScopedConn sub(ctx.server()), pub(ctx.server());
    ASSERT(ctx, sub.ok() && pub.ok(), "connect failed");
    unsigned err = 0;
    Rng rng(ctx.seed);
    const char* EVT = "EVT_DELAY";
    const int DELAY = 150;  // ms

    REQUIRE_OK(ctx, subscribedelaypost(sub.fd(), fx::TAG_POST, EVT, DELAY, &err), err);
    publish(ctx, pub.fd(), 55, rng, /*post=*/true);  // 触发 → 定时 DELAY ms 后投递

    Recv r = receive(sub.fd(), 3000);  // 足够长，等延迟投递到来
    ASSERT(ctx, r.ok && r.err == 0, "delaypost recv: ret=%d err=%u(%s)", (int)r.ok, r.err, err_name(r.err));
    // 关键语义：延迟投递回传的是 eventname，而非被写 tag。
    CHECK(ctx, strcmp(r.tag, EVT) == 0, "delaypost tagname='%s' want '%s'", r.tag, EVT);
    std::string perr;
    CHECK(ctx, check_payload(r.val.data(), POST_SZ, 55, &perr), "delaypost payload: %s", perr.c_str());
    // 确实延迟了（下界略小于 DELAY 以容忍计时粒度）。
    CHECK(ctx, r.ms >= 100.0, "delaypost delivered too early: %.1f ms (delay=%d)", r.ms, DELAY);
}

// --- [B] 重复订阅同 tag（无去重）→ 一次 writeb 收到两份 ---------------------
TEST("pubsub.duplicate_subscribe", TAG_PUBSUB)
{
    ScopedConn sub(ctx.server()), pub(ctx.server());
    ASSERT(ctx, sub.ok() && pub.ok(), "connect failed");
    unsigned err = 0;
    Rng rng(ctx.seed);

    // 同连接订阅两次同一 tag：实现不去重 → 两个 EventNode。
    REQUIRE_OK(ctx, subscribe(sub.fd(), fx::TAG_POST, &err), err);
    REQUIRE_OK(ctx, subscribe(sub.fd(), fx::TAG_POST, &err), err);
    publish(ctx, pub.fd(), 88, rng, /*post=*/true);  // 一次 writeb → 两份投递

    for (int i = 0; i < 2; i++) {
        Recv r = receive(sub.fd(), 1500);
        CHECK(ctx, r.ok && r.err == 0 && strcmp(r.tag, fx::TAG_POST) == 0,
              "duplicate delivery %d: ret=%d err=%u(%s) tag=%s", i, (int)r.ok, r.err, err_name(r.err), r.tag);
    }
    // 不应有第三份：再等一个短超时应 WAIT_TIMEOUT。
    Recv r3 = receive(sub.fd(), 200);
    CHECK(ctx, r3.ok && r3.err == ERROR_WAIT_TIMEOUT, "expected exactly 2 deliveries; 3rd err=%u(%s)",
          r3.err, err_name(r3.err));
}

// --- [B] 订阅不存在的 tag：应校验并拒绝（现实现不校验，接受）→ XFAIL#P1 -----
// 断言的是“正确行为”：订阅不存在 tag 应失败并置 TAG_NOT_EXIST。
// 现实现 HandleSubscribe 不校验、直接 Attach 并回成功 → 今日必失败 → 记 XFAIL；
// 一旦服务端补上校验 → 转 XPASS，提醒更新登记表。
TEST_XFAIL("pubsub.subscribe_nonexistent", TAG_PUBSUB, BUG_P1)
{
    ScopedConn sub(ctx.server());
    ASSERT(ctx, sub.ok(), "connect failed");
    unsigned err = 0;
    bool ok = subscribe(sub.fd(), "NO_SUCH_TAG_PQR", &err);
    ASSERT(ctx, ok || err == ERROR_TAG_NOT_EXIST, "unexpected subscribe error %u", err);
    BUG_CHECK(ctx, !ok && err == ERROR_TAG_NOT_EXIST,
              "P1: nonexistent subscription accepted (ret=%d err=%u)", ok, err);
}

// --- [X][XFAIL P2] 单连接订阅 >500 → NotifySubscriber 触发 server exit(1) ------
// 机理（ngx_c_slogic.cxx:620）：NotifySubscriber 里 `if (usernumber > 500) { ...; exit(1); }`
//   作为“事件风暴保护”，但它直接 exit 整个 worker → master 收 SIGCHLD 一并退出（无重生）。
//   单条连接订阅同一 tag 501 次（HandleSubscribe 不去重、不限量），一次 writeb 即令 usernumber=501。
// 断言正确行为：订阅过多不应打崩整个 server（应拒绝新订阅或仅记日志）。今日必崩 → XFAIL。
TEST_XFAIL("pubsub.subscribe_storm_exit", TAG_PUBSUB | TAG_DESTRUCTIVE, BUG_P2)
{
    ServerConfig cfg;
    cfg.threads = 2;
    cfg.asan = ctx.expectAsan;
    cfg.allowAsanErrors = true;
    cfg.allowLeaks = true;
    ServerFixture srv(cfg);
    ASSERT(ctx, srv.start(), "private fixture start failed: %s", srv.startup_error().c_str());

    unsigned err = 0;
    ScopedConn sub(srv);
    ASSERT(ctx, sub.ok(), "subscriber connect failed");
    // 同一连接订阅同一 tag 501 次 → GetSubscriber(TAG_POST).size()==501。
    for (int i = 0; i < 501; i++) {
        REQUIRE_OK(ctx, subscribe(sub.fd(), fx::TAG_POST, &err), err);
    }
    // 另一连接 writeb(TAG_POST) → NotifySubscriber(usernumber=501>500) → exit(1)。
    {
        ScopedConn pub(srv);
        ASSERT(ctx, pub.ok(), "publisher connect failed");
        std::vector<char> payload(POST_SZ, 0x5A);
        writeb(pub.fd(), fx::TAG_POST, payload.data(), POST_SZ, &err);  // 触发风暴保护
    }
    bool died = srv.wait_stopped(3000);
    BUG_CHECK(ctx, !died,
              "P2: >500 subscribers triggered server exit(1) —— 事件风暴保护打崩整个 server");
}

// --- [内存][P3 特征化] 只订不取 → m_listPost 无界堆积（RSS 趋势，仅报告）---------
// 机理（登记表 P3）：订阅者从不 waitpostdata，持续 writeb 触发投递，投递数据进该连接的
//   m_listPost 且永不 drain → 可达内存无界增长（LSan 看不见）。
// 与 M1 一致：RSS 能否回落依赖 glibc malloc_trim，不宜作断言——此处仅特征化报告趋势，
//   可靠门控只有“server 存活、数据面无损”。
TEST("pubsub.listpost_unbounded_trend", TAG_PUBSUB | TAG_MEMORY)
{
    ServerConfig cfg;
    cfg.threads = 2;
    cfg.asan = ctx.expectAsan;
    ServerFixture srv(cfg);
    ASSERT(ctx, srv.start(), "private fixture start failed: %s", srv.startup_error().c_str());
    pid_t wpid = srv.worker_pid();
    ASSERT(ctx, wpid > 0, "cannot resolve worker pid for sampling");

    unsigned err = 0;
    ScopedConn sub(srv);        // 订阅者：只订阅，从不 waitpostdata
    ASSERT(ctx, sub.ok(), "sub connect failed");
    REQUIRE_OK(ctx, subscribe(sub.fd(), fx::TAG_POST, &err), err);
    ScopedConn pub(srv);
    ASSERT(ctx, pub.ok(), "pub connect failed");

    ProcSampler smp(wpid);
    ASSERT(ctx, smp.tick(), "baseline sample failed");
    const long rss_base = smp.rss_last_kb();

    const int N = 4000 * ctx.scale;
    Rng rng(ctx.seed);
    for (int i = 0; i < N; i++) {
        std::vector<char> payload;
        make_payload(payload, POST_SZ, (uint32_t)i, rng);
        REQUIRE_OK(ctx, writeb(pub.fd(), fx::TAG_POST, payload.data(), POST_SZ, &err), err);
        if ((i % 200) == 199) smp.tick();
    }
    smp.tick();
    const long rss_end = smp.rss_last_kb();
    smp.report("listpost-growth");
    printf("    [note] 只订不取 → m_listPost 无界堆积（登记表 P3）：RSS base=%ldKB end=%ldKB Δ=%ldKB "
           "（特征化报告，非门控）\n", rss_base, rss_end, rss_end - rss_base);

    CHECK(ctx, srv.running(), "server died while m_listPost accumulated %d undrained posts", N);
}
