// test_board.cpp —— BOARD 基础与边界用例（方案 §7.1 / §7.2 的 [N]/[B] 象限起步）
//
// 覆盖 API：writeb / readb / writeb_notpost（readb_string/writeb_string 见 test_string.cpp）。
//
// 共享 server 纪律（重要）：这些用例跑在 Runner 的“共享 server”上，夹具(fx::)为全体用例共用。
//   因此：① 绝不 clearb（会清空整块板，毁掉其它用例的夹具）；② 只删自己建的 tag，绝不删 fx 夹具 tag。
//   需要清板/私有板的场景请自建 ServerFixture（见 test_memory/test_churn）。
#include <unistd.h>

#include <cstring>
#include <vector>

#include "framework/ts.h"
#include "cases/xfail_ids.h"

using namespace ts;

namespace {

// 通用“写入→读回→逐字节比对”。写读任一步失败即硬中止（前置条件不满足，后续比对无意义）。
void roundtrip(TestContext& ctx, int fd, const char* tag,
               const void* in, int sz, const char* what)
{
    unsigned err = 0;
    std::vector<char> out(sz, 0);
    REQUIRE_OK(ctx, writeb(fd, tag, const_cast<void*>(in), sz, &err), err);
    REQUIRE_OK(ctx, readb(fd, tag, out.data(), sz, &err, nullptr), err);
    CHECK(ctx, memcmp(in, out.data(), sz) == 0, "%s: readback mismatch", what);
}

}  // namespace

// --- [N] 正常路径：各标量类型写读一致 -------------------------------------
TEST("board.scalar_roundtrip", TAG_BOARD)
{
    ScopedConn c(ctx.server());
    ASSERT(ctx, c.ok(), "connectgplat failed");
    int fd = c.fd();
    Rng rng(ctx.seed);

    uint8_t  b = (uint8_t)rng.range(0, 1);
    int16_t  i16 = (int16_t)rng.next_u32();
    int32_t  i32 = (int32_t)rng.next_u32();
    int64_t  i64 = (int64_t)rng.next_u64();
    float    f32; rng.fill(&f32, sizeof(f32));
    double   f64; rng.fill(&f64, sizeof(f64));

    roundtrip(ctx, fd, fx::TAG_BOOL, &b,   sizeof(b),   "BOOL");
    roundtrip(ctx, fd, fx::TAG_I16,  &i16, sizeof(i16), "I16");
    roundtrip(ctx, fd, fx::TAG_I32,  &i32, sizeof(i32), "I32");
    roundtrip(ctx, fd, fx::TAG_I64,  &i64, sizeof(i64), "I64");
    // 浮点按位比较（填充的是任意字节；NaN 也要求位级一致，正是我们要的）
    roundtrip(ctx, fd, fx::TAG_F32,  &f32, sizeof(f32), "F32");
    roundtrip(ctx, fd, fx::TAG_F64,  &f64, sizeof(f64), "F64");
}

// --- [N] 数组标量：Single[4] 整体写读一致 ---------------------------------
TEST("board.array_roundtrip", TAG_BOARD)
{
    ScopedConn c(ctx.server());
    ASSERT(ctx, c.ok(), "connectgplat failed");
    Rng rng(ctx.seed);

    float arr[4];
    for (float& v : arr) rng.fill(&v, sizeof(v));
    roundtrip(ctx, c.fd(), fx::TAG_ARR, arr, (int)sizeof(arr), "ARR_F32_4");
}

// --- [N] writeb_notpost：不触发订阅投递，但写入的值照样可读回 --------------
TEST("board.notpost_roundtrip", TAG_BOARD)
{
    ScopedConn c(ctx.server());
    ASSERT(ctx, c.ok(), "connectgplat failed");
    int fd = c.fd();
    unsigned err = 0;

    int32_t v = (int32_t)Rng(ctx.seed).next_u32();
    REQUIRE_OK(ctx, writeb_notpost(fd, fx::TAG_I32, &v, sizeof(v), &err), err);
    int32_t rd = ~v;
    REQUIRE_OK(ctx, readb(fd, fx::TAG_I32, &rd, sizeof(rd), &err, nullptr), err);
    CHECK(ctx, rd == v, "notpost value not stored: wrote %d read %d", v, rd);
}

// --- [N] 二进制负载（带 CRC 自校验）：常见尺寸写读一致 ---------------------
TEST("board.binary_roundtrip", TAG_BOARD)
{
    ScopedConn c(ctx.server());
    ASSERT(ctx, c.ok(), "connectgplat failed");
    int fd = c.fd();
    Rng rng(ctx.seed);

    struct Case { const char* tag; int size; } cases[] = {
        {fx::TAG_BIN256, 256}, {fx::TAG_BIN4K, 4096},
    };
    for (auto& cs : cases) {
        unsigned err = 0;
        std::vector<char> payload;
        make_payload(payload, cs.size, 1, rng);
        REQUIRE_OK(ctx, writeb(fd, cs.tag, payload.data(), cs.size, &err), err);

        std::vector<char> rd(cs.size, 0);
        REQUIRE_OK(ctx, readb(fd, cs.tag, rd.data(), cs.size, &err, nullptr), err);
        std::string perr;
        CHECK(ctx, check_payload(rd.data(), cs.size, 1, &perr),
              "%s(%d): %s", cs.tag, cs.size, perr.c_str());
    }
}

// --- [B] 满包边界：16384 字节（=MAXMSGLEN）单次写读 ------------------------
TEST("board.binary_maxlen", TAG_BOARD)
{
    ScopedConn c(ctx.server());
    ASSERT(ctx, c.ok(), "connectgplat failed");
    int fd = c.fd();
    Rng rng(ctx.seed);
    unsigned err = 0;

    const int N = 16384;
    std::vector<char> payload;
    make_payload(payload, N, 7, rng);
    REQUIRE_OK(ctx, writeb(fd, fx::TAG_BINMAX, payload.data(), N, &err), err);

    std::vector<char> rd(N, 0);
    REQUIRE_OK(ctx, readb(fd, fx::TAG_BINMAX, rd.data(), N, &err, nullptr), err);
    std::string perr;
    CHECK(ctx, check_payload(rd.data(), N, 7, &perr), "maxlen payload: %s", perr.c_str());
}

// --- [I] 非法：读一个不存在的 tag → 失败且置 TAG_NOT_EXIST（不应挂死/崩溃） --
TEST("board.read_missing_tag", TAG_BOARD)
{
    ScopedConn c(ctx.server());
    ASSERT(ctx, c.ok(), "connectgplat failed");
    unsigned err = 0;
    char buf[32] = {0};
    // 期望 ret=false 且 error=1042(TAG_NOT_EXIST)（实测确认）。
    EXPECT_ERR(ctx, readb(c.fd(), "NO_SUCH_TAG_xyz", buf, sizeof(buf), &err, nullptr),
               err, ERROR_TAG_NOT_EXIST);
}

// --- [B] 超容量写：向 256B tag 写 1024B → RECORDSIZE 拒绝（不应越界写坏内存） --
TEST("board.write_oversize", TAG_BOARD)
{
    ScopedConn c(ctx.server());
    ASSERT(ctx, c.ok(), "connectgplat failed");
    unsigned err = 0;
    std::vector<char> big(1024, 0x5A);
    // 期望 ret=false 且 error=1014(RECORDSIZE)（实测确认）。
    EXPECT_ERR(ctx, writeb(c.fd(), fx::TAG_BIN256, big.data(), (int)big.size(), &err),
               err, ERROR_RECORDSIZE);
}

// ===========================================================================
// 破坏性 createtag 边界（方案 §7.2[X]；原登记表 C2/C3，均已修复）。全部自建私有 server，
// 绝不碰共享实例；会打崩 server 的用例让私有实例随 teardown 回收。
// ===========================================================================

// 私有 server（独立 sandbox + 自动空闲端口）。crash 变体额外放行 ASan 错误/泄漏，
// 使“崩溃”这一现象被记为 XFAIL 而非 teardown 的硬失败。
#define PRIVATE_BOARD_SERVER(SRV, ALLOW_CRASH)                                   \
    ServerConfig SRV##_cfg;                                                      \
    SRV##_cfg.threads = 2;                                                       \
    SRV##_cfg.asan = ctx.expectAsan;                                            \
    SRV##_cfg.allowAsanErrors = (ALLOW_CRASH);                                  \
    SRV##_cfg.allowLeaks = (ALLOW_CRASH);                                       \
    ServerFixture SRV(SRV##_cfg);                                              \
    ASSERT(ctx, SRV.start(), "private fixture start failed: %s", SRV.startup_error().c_str())

// --- [X] createtag(typesize==0)：客户端参数校验拒绝，返回 ERROR_PARAMETER_SIZE 且不关 socket ---
// （原缺陷 C1 已修复：以前 send_all(type,0) 返回 0 被误判为发送失败并 close(socket)。）
TEST("board.createtag_typesize_zero", TAG_BOARD | TAG_DESTRUCTIVE)
{
    PRIVATE_BOARD_SERVER(srv, /*ALLOW_CRASH=*/false);
    int fd = srv.open_conn();
    ASSERT(ctx, fd >= 0, "connect failed");

    unsigned err = 0;
    char dummy = 0;
    EXPECT_ERR(ctx, createtag(fd, "C1_NOTYPE", 16, &dummy, 0, &err), err, ERROR_PARAMETER_SIZE);

    // socket 必须仍可用：同一连接上带类型的 createtag 应成功。
    char type = 1;
    EXPECT_OK(ctx, createtag(fd, "C1_WITHTYPE", 16, &type, 1, &err), err);
    disconnectgplat(fd);
}

// --- [X] createtag(负 tagsize)：客户端参数校验拒绝，返回 ERROR_PARAMETER_SIZE 且不关 socket ---
// （原缺陷 C2 已修复：CreateItem 对负 itemSize 放行 → memset 超大长度使 server 崩溃。）
TEST("board.createtag_negative_tagsize", TAG_BOARD | TAG_DESTRUCTIVE)
{
    PRIVATE_BOARD_SERVER(srv, /*ALLOW_CRASH=*/false);
    int fd = srv.open_conn();
    ASSERT(ctx, fd >= 0, "connect failed");

    unsigned err = 0;
    char type = 1;
    EXPECT_ERR(ctx, createtag(fd, "C2_NEG", -1, &type, 1, &err), err, ERROR_PARAMETER_SIZE);
    EXPECT_OK(ctx, createtag(fd, "C2_OK", 16, &type, 1, &err), err);
    disconnectgplat(fd);
    CHECK(ctx, srv.running(), "server died after negative tagsize");
}

// --- [X] 裸协议绕过客户端校验：CREATEITEM(recsize<0) → server 自身必须拒绝，不得崩溃 ---
TEST("board.createtag_negative_tagsize_raw", TAG_BOARD | TAG_DESTRUCTIVE)
{
    PRIVATE_BOARD_SERVER(srv, /*ALLOW_CRASH=*/false);
    int fd = raw_connect(srv.ip(), srv.port(), 2000);
    ASSERT(ctx, fd >= 0, "raw connect failed");

    char type = 1;
    MSGHEAD h = make_head(CREATEITEM, "BOARD", "C2_RAW");
    h.recsize = -1;
    h.bodysize = 1;
    ASSERT(ctx, send_msg(fd, h, &type, 1), "send CREATEITEM failed");

    MSGHEAD out{};
    std::vector<char> body;
    RecvStatus rs = recv_msg(fd, out, body);
    close(fd);
    CHECK(ctx, rs == RecvStatus::OK, "no response to negative-recsize CREATEITEM (status=%d)", (int)rs);
    CHECK(ctx, out.error == ERROR_PARAMETER_SIZE, "expected ERROR_PARAMETER_SIZE, got %u", (unsigned)out.error);
    CHECK(ctx, srv.running(), "server died on negative recsize (public-API DoS)");
}

// --- [X] createtag(负 typesize)：客户端参数校验干净拒绝（false + PARAMETER_SIZE），不崩溃/不挂死 ---
// （原缺陷 C3 已修复：以前 typesize<0 未校验，send_all 以超大 size_t 越界读。）
// fork 隔离 + 看门狗，防止回归时拖垮套件。
TEST("board.createtag_negative_typesize", TAG_BOARD | TAG_DESTRUCTIVE)
{
    PRIVATE_BOARD_SERVER(srv, /*ALLOW_CRASH=*/false);
    const std::string ip = srv.ip();
    const int port = srv.port();

    ForkResult r = run_in_fork([&]() {
        int fd = connectgplat(ip.c_str(), port);
        if (fd < 0) _exit(77);
        unsigned err = 0;
        char type = 1;
        bool ok = createtag(fd, "C3_NEG", 16, &type, -1, &err);
        _exit((!ok && err == ERROR_PARAMETER_SIZE) ? 0 : 2);
    }, 4000);

    ASSERT(ctx, !(r.exited && r.exit_code == 77), "C3 child could not connect");
    CHECK(ctx, r.exited && r.exit_code == 0,
          "negative typesize mishandled client-side (%s)", r.describe());
    CHECK(ctx, srv.running(), "server died after negative typesize");
}
