// test_getresponse.cpp —— 请求/响应 getresponse（方案 §7.7 非破坏象限）
//
// 机制（已对照 gplat/ngx_c_slogic.cxx: HandleGetResponse/StartRequest/DeliverResponse 确认）：
//   请求方 getresponse(reqTag, req, reqSize, rspTag, rsp, rspSize, timeout)：
//     1) 服务端校验：reqTag/rspTag 非空且不相等、timeout>0、reqSize>0 且 ==body、rspSize∈(0,MAXMSGLEN]；
//     2) 再用 ReadB 校验两 tag 存在且大小一致（reqSize==itemsize(reqTag)、rspSize==itemsize(rspTag)）；
//     3) 入 reqTag 的通道：无 active 则置为 active 并 WriteB(reqTag)+NotifySubscriber；否则进 waiting 队列；
//        active 存在且 waiting 已达 REQUEST_QUEUE_MAX(64) → REQUEST_QUEUE_FULL；
//        同一 rspTag 被不同 reqTag 复用 → INVALID_PARAMETER。
//   响应方：subscribe(reqTag) → waitpostdata 收到请求 → 计算 → 平凡 writeb(rspTag, rsp)（start==1）
//        → 服务端 DeliverResponse 把响应回投给阻塞的请求方（不通知普通订阅者）。
//   超时：到点 OnRequestTimeout → 请求方收到 ERROR_RESPONSE_TIMEOUT。
//
// 关键尺寸约束：reqSize 必须 == REQ_TAG 的 itemsize(256)，rspSize 必须 == RSP_TAG 的 itemsize(256)。
// 跨连接前提：sandbox WorkerProcesses=1（单 worker 进程），请求通道/订阅进程内共享。
#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "framework/ts.h"
#include "cases/xfail_ids.h"

using namespace ts;

namespace {

constexpr int REQ_SZ = 256;    // REQ_TAG / RSP_TAG 的 itemsize（char[256]）
constexpr int VALBUF = 512;    // 响应方收包缓冲
constexpr int REQUEST_QUEUE_MAX = 64;              // 服务端 waiting 队列上限（与源码常量一致）
constexpr int OVERFLOW_TOTAL = REQUEST_QUEUE_MAX + 2;  // 66 = 1 active + 64 waiting + 1 overflow

// 后台响应方：订阅 reqTag，循环 waitpostdata；收到请求后把同样的数据回写 rspTag（回显），
// 从而让请求方能用自带的 CRC/序列号校验整条往返链路。以 stop 原子量优雅退出。
class Responder {
public:
    void start(ServerFixture& s, const char* reqTag, const char* rspTag)
    {
        srv_ = &s; req_ = reqTag; rsp_ = rspTag;
        th_ = std::thread([this] { run(); });
    }
    // 等待响应方完成 subscribe（服务端已登记），避免请求早于订阅导致投递落空。
    bool wait_ready(int timeout_ms)
    {
        uint64_t t0 = now_nanos();
        while (!ready_.load()) {
            if (failed_.load()) return false;
            if ((now_nanos() - t0) / 1000000 > (uint64_t)timeout_ms) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    }
    int served() const { return served_.load(); }
    void stop_join()
    {
        stop_.store(true);
        if (th_.joinable()) th_.join();
    }

private:
    void run()
    {
        int fd = srv_->open_conn();
        if (fd < 0) { failed_.store(true); return; }
        unsigned err = 0;
        if (!subscribe(fd, req_.c_str(), &err)) { failed_.store(true); disconnectgplat(fd); return; }
        ready_.store(true);

        char name[64];
        std::vector<char> buf(VALBUF, 0);
        while (!stop_.load()) {
            err = 0;
            bool ok = waitpostdata(fd, name, sizeof(name), buf.data(), VALBUF, 120, &err);
            if (!ok) break;  // 连接层错误：收尾退出
            if (err == ERROR_WAIT_TIMEOUT) continue;
            if (strcmp(name, req_.c_str()) != 0) continue;  // 只应答请求 tag
            unsigned werr = 0;
            writeb(fd, rsp_.c_str(), buf.data(), REQ_SZ, &werr);  // 回显 → DeliverResponse
            served_.fetch_add(1);
        }
        disconnectgplat(fd);
    }

    ServerFixture* srv_ = nullptr;
    std::string req_, rsp_;
    std::thread th_;
    std::atomic<bool> stop_{false}, ready_{false}, failed_{false};
    std::atomic<int> served_{0};
};

}  // namespace

// --- [N] 基本请求/响应：回显往返，数据 CRC 校验 ---------------------------
TEST("getresp.basic", TAG_GETRESP)
{
    Responder responder;
    responder.start(ctx.server(), fx::REQ_TAG, fx::RSP_TAG);
    ASSERT(ctx, responder.wait_ready(3000), "responder failed to subscribe");

    ScopedConn c(ctx.server());
    ASSERT(ctx, c.ok(), "connect failed");
    unsigned err = 0;
    Rng rng(ctx.seed);

    std::vector<char> req;
    make_payload(req, REQ_SZ, 123, rng);
    std::vector<char> rsp(REQ_SZ, 0);
    bool ok = getresponse(c.fd(), fx::REQ_TAG, req.data(), REQ_SZ, fx::RSP_TAG,
                          rsp.data(), REQ_SZ, &err, 3000);
    // 先停响应方再断言，确保无论成败都回收线程。
    responder.stop_join();

    ASSERT(ctx, ok && err == 0, "getresponse: ret=%d err=%u(%s)", (int)ok, err, err_name(err));
    CHECK(ctx, memcmp(req.data(), rsp.data(), REQ_SZ) == 0, "echo mismatch (response != request)");
    CHECK(ctx, responder.served() >= 1, "responder served=%d (want >=1)", responder.served());
}

// --- [N] 连续多次请求：同一通道 active→下一个 的推进正确 -------------------
TEST("getresp.sequential_many", TAG_GETRESP)
{
    Responder responder;
    responder.start(ctx.server(), fx::REQ_TAG, fx::RSP_TAG);
    ASSERT(ctx, responder.wait_ready(3000), "responder failed to subscribe");

    ScopedConn c(ctx.server());
    ASSERT(ctx, c.ok(), "connect failed");
    Rng rng(ctx.seed);
    const int N = 20;
    int good = 0;

    for (int i = 0; i < N; i++) {
        unsigned err = 0;
        std::vector<char> req;
        make_payload(req, REQ_SZ, (uint32_t)(1000 + i), rng);
        std::vector<char> rsp(REQ_SZ, 0);
        bool ok = getresponse(c.fd(), fx::REQ_TAG, req.data(), REQ_SZ, fx::RSP_TAG,
                              rsp.data(), REQ_SZ, &err, 3000);
        if (ok && err == 0 && memcmp(req.data(), rsp.data(), REQ_SZ) == 0) good++;
        else TS_FAILF(ctx, "req %d failed: ret=%d err=%u(%s)", i, (int)ok, err, err_name(err));
        ctx.checks++;
    }
    responder.stop_join();
    CHECK(ctx, good == N, "sequential good=%d/%d", good, N);
}

// --- [I] 客户端侧非法入参 → INVALID_PARAMETER（不触碰 socket，可连用） ------
TEST("getresp.invalid_params", TAG_GETRESP)
{
    ScopedConn c(ctx.server());
    ASSERT(ctx, c.ok(), "connect failed");
    int fd = c.fd();
    unsigned err = 0;
    std::vector<char> req(REQ_SZ, 0x11), rsp(REQ_SZ, 0);

    // timeout_ms <= 0
    EXPECT_ERR(ctx, getresponse(fd, fx::REQ_TAG, req.data(), REQ_SZ, fx::RSP_TAG, rsp.data(), REQ_SZ, &err, 0),
               err, ERROR_INVALID_PARAMETER);
    // request_size <= 0
    EXPECT_ERR(ctx, getresponse(fd, fx::REQ_TAG, req.data(), 0, fx::RSP_TAG, rsp.data(), REQ_SZ, &err, 1000),
               err, ERROR_INVALID_PARAMETER);
    // response_size <= 0
    EXPECT_ERR(ctx, getresponse(fd, fx::REQ_TAG, req.data(), REQ_SZ, fx::RSP_TAG, rsp.data(), 0, &err, 1000),
               err, ERROR_INVALID_PARAMETER);
}

// --- [I] 尺寸超 MAXMSGLEN → PARAMETER_SIZE（客户端侧） ---------------------
TEST("getresp.oversize_param", TAG_GETRESP)
{
    ScopedConn c(ctx.server());
    ASSERT(ctx, c.ok(), "connect failed");
    int fd = c.fd();
    unsigned err = 0;
    std::vector<char> req(REQ_SZ, 0x22), rsp(REQ_SZ, 0);

    EXPECT_ERR(ctx, getresponse(fd, fx::REQ_TAG, req.data(), GPLAT_MAX_DATA_SIZE + 1, fx::RSP_TAG,
                                rsp.data(), REQ_SZ, &err, 1000), err, ERROR_PARAMETER_SIZE);
    EXPECT_ERR(ctx, getresponse(fd, fx::REQ_TAG, req.data(), REQ_SZ, fx::RSP_TAG,
                                rsp.data(), GPLAT_MAX_DATA_SIZE + 1, &err, 1000), err, ERROR_PARAMETER_SIZE);
}

// --- [B] req/resp 同 tag → 服务端 INVALID_PARAMETER -----------------------
TEST("getresp.same_req_resp_tag", TAG_GETRESP)
{
    ScopedConn c(ctx.server());
    ASSERT(ctx, c.ok(), "connect failed");
    unsigned err = 0;
    std::vector<char> req(REQ_SZ, 0x33), rsp(REQ_SZ, 0);
    EXPECT_ERR(ctx, getresponse(c.fd(), fx::REQ_TAG, req.data(), REQ_SZ, fx::REQ_TAG,
                                rsp.data(), REQ_SZ, &err, 1000), err, ERROR_INVALID_PARAMETER);
}

// --- [B] 不存在的 request tag → 服务端 ReadB 校验失败 → TAG_NOT_EXIST(1042) ----
TEST("getresp.nonexistent_tag", TAG_GETRESP)
{
    ScopedConn c(ctx.server());
    ASSERT(ctx, c.ok(), "connect failed");
    unsigned err = 0;
    std::vector<char> req(REQ_SZ, 0x44), rsp(REQ_SZ, 0);
    bool ok = getresponse(c.fd(), "NO_SUCH_REQ_T", req.data(), REQ_SZ, fx::RSP_TAG,
                          rsp.data(), REQ_SZ, &err, 1000);
    CHECK(ctx, !ok && err == ERROR_TAG_NOT_EXIST, "nonexistent req tag: ret=%d err=%u(%s)",
          (int)ok, err, err_name(err));
}

// --- [B] request_size 与 tag itemsize 不符 → 服务端 RECORDSIZE(1014) ---------
TEST("getresp.wrong_size", TAG_GETRESP)
{
    ScopedConn c(ctx.server());
    ASSERT(ctx, c.ok(), "connect failed");
    unsigned err = 0;
    std::vector<char> req(128, 0x55), rsp(REQ_SZ, 0);
    bool ok = getresponse(c.fd(), fx::REQ_TAG, req.data(), 128, fx::RSP_TAG,
                          rsp.data(), REQ_SZ, &err, 1000);
    CHECK(ctx, !ok && err == ERROR_RECORDSIZE, "reqSize=128 vs itemsize=256: ret=%d err=%u(%s)",
          (int)ok, err, err_name(err));
}

// --- [B] 无人应答 → ERROR_RESPONSE_TIMEOUT，且约在 timeout 时返回 ----------
TEST("getresp.timeout", TAG_GETRESP)
{
    ScopedConn c(ctx.server());
    ASSERT(ctx, c.ok(), "connect failed");
    unsigned err = 0;
    std::vector<char> req(REQ_SZ, 0x66), rsp(REQ_SZ, 0);

    uint64_t t0 = now_nanos();
    bool ok = getresponse(c.fd(), fx::REQ_TAG, req.data(), REQ_SZ, fx::RSP_TAG,
                          rsp.data(), REQ_SZ, &err, 300);
    double ms = (double)(now_nanos() - t0) / 1e6;
    CHECK(ctx, !ok && err == ERROR_RESPONSE_TIMEOUT, "no-responder: ret=%d err=%u(%s)",
          (int)ok, err, err_name(err));
    CHECK(ctx, ms >= 200.0 && ms < 3000.0, "timeout returned at %.1f ms (want ~300)", ms);
}

// --- [B] 并发排队上限：恰好 1 个请求被 REQUEST_QUEUE_FULL 拒 ----------------
// 同一 reqTag 上并发 (1 active + REQUEST_QUEUE_MAX waiting) = 65 个被接受并最终超时；
// 第 66 个（无论线程调度顺序，服务端在 m_reqMutex 下串行处理 → 恰有一个撞到上限）→ FULL。
TEST("getresp.concurrency_queue_full", TAG_GETRESP)
{
    ServerFixture& srv = ctx.server();
    const int TOTAL = OVERFLOW_TOTAL;  // 66 = 1 active + 64 waiting + 1 overflow
    std::vector<std::thread> ths;
    std::atomic<int> nFull{0}, nTimeout{0}, nOther{0};

    for (int i = 0; i < TOTAL; i++) {
        ths.emplace_back([&srv, &nFull, &nTimeout, &nOther, seed = ctx.seed, i] {
            int fd = srv.open_conn();
            if (fd < 0) { nOther.fetch_add(1); return; }
            unsigned err = 0;
            Rng rng(seed ^ (uint64_t)(i + 1));
            std::vector<char> req;
            make_payload(req, REQ_SZ, (uint32_t)i, rng);
            std::vector<char> rsp(REQ_SZ, 0);
            // 无响应方：被接受者将等到超时；给足并发注册的窗口（1500ms）。
            bool ok = getresponse(fd, fx::REQ_TAG, req.data(), REQ_SZ, fx::RSP_TAG,
                                  rsp.data(), REQ_SZ, &err, 1500);
            if (!ok && err == ERROR_REQUEST_QUEUE_FULL) nFull.fetch_add(1);
            else if (!ok && err == ERROR_RESPONSE_TIMEOUT) nTimeout.fetch_add(1);
            else nOther.fetch_add(1);
            disconnectgplat(fd);
        });
    }
    for (auto& t : ths) t.join();

    printf("    [info] concurrency: full=%d timeout=%d other=%d (total=%d)\n",
           nFull.load(), nTimeout.load(), nOther.load(), TOTAL);
    CHECK(ctx, nFull.load() == 1, "want exactly 1 REQUEST_QUEUE_FULL, got %d", nFull.load());
    CHECK(ctx, nTimeout.load() == TOTAL - 1, "want %d timeouts, got %d", TOTAL - 1, nTimeout.load());
    CHECK(ctx, nOther.load() == 0, "unexpected other outcomes: %d", nOther.load());
}

// --- [内存][XFAIL G1] m_mapResponseOwner 永不 erase（确定性探针，非 RSS 趋势）-----
// 机理（ngx_c_slogic.cxx:1350）：StartRequest 写 `m_mapResponseOwner[responseTag]=requestTag`，
//   但全文件再无任何 .erase —— 请求即便已解决（响应/超时）该归属也永久保留。后果有二：
//   ① 该 response_tag 再不能被别的 request_tag 复用；② 不同 response_tag 无界累积（LSan 看不见）。
// 本探针用“复用性”做**确定性**判定（优于 RSS）：第一请求解决后，RSP_A 理应可被新 REQ_B 复用。
// 断言正确行为：复用不应因陈旧归属被拒。今日必被 INVALID_PARAMETER 拒绝 → XFAIL。
TEST_XFAIL("getresp.response_owner_never_erased", TAG_GETRESP | TAG_MEMORY, BUG_G1)
{
    ServerConfig cfg;
    cfg.threads = 2;
    cfg.asan = ctx.expectAsan;
    ServerFixture srv(cfg);
    ASSERT(ctx, srv.start(), "private fixture start failed: %s", srv.startup_error().c_str());

    ScopedConn c(srv);
    ASSERT(ctx, c.ok(), "connect failed");
    unsigned err = 0;
    std::vector<char> req(REQ_SZ, 0x11), rsp(REQ_SZ, 0);

    // 第一次请求无响应方 → 由服务端定时器判超时解决；但 StartRequest 已登记 RSP_A→REQ_A。
    bool ok1 = getresponse(c.fd(), fx::REQ_TAG, req.data(), REQ_SZ, fx::RSP_TAG,
                           rsp.data(), REQ_SZ, &err, 300);
    ASSERT(ctx, !ok1 && err == ERROR_RESPONSE_TIMEOUT,
           "first request should time out (no responder): ok=%d err=%u(%s)", (int)ok1, err, err_name(err));

    // 新建第二个 request tag（与 RSP_A 同尺寸 256），尝试复用 RSP_A 作为响应 tag。
    char type = 1;
    REQUIRE_OK(ctx, createtag(c.fd(), "G1_REQ_B", REQ_SZ, &type, 1, &err), err);

    unsigned err2 = 0;
    bool ok2 = getresponse(c.fd(), "G1_REQ_B", req.data(), REQ_SZ, fx::RSP_TAG,
                           rsp.data(), REQ_SZ, &err2, 300);
    // 若已修复（归属在解决后释放）：REQ_B 复用 RSP_A 应进入等待并最终超时（无响应方）。
    ASSERT(ctx, !ok2, "second request unexpectedly succeeded");
    BUG_CHECK(ctx, err2 != ERROR_INVALID_PARAMETER,
              "G1: RSP_A still owned by REQ_A after the first request resolved "
              "(m_mapResponseOwner never erased); err=%u(%s)", err2, err_name(err2));
}
