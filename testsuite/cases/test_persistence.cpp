// test_persistence.cpp —— 持久化 / 重启（方案 §7.8）
//
// 核心思路：QBD 文件是 mmap(MAP_SHARED) 落盘的，数据活在页缓存里并由内核写回。
//   因此只要内核不崩，BOARD/QUEUE 的内容应当跨进程重启存活——无论优雅 SIGTERM 还是 SIGKILL。
//   本模块用「写入已知快照 → 停机 → 复起 → 读回比对」证明这一点，并校验队列读写指针自洽。
//
// 与众不同之处：这些用例**不**复用共享 server（它们要停/起服务器）。每个用例在栈上构造一个
//   私有 ServerFixture（独立 sandbox + 自动探测的空闲端口，绝不碰 8777 生产实例或共享实例），
//   start() 建好标准 fixtures，relaunch() 复用同一 qbdfile 但不重建 fixtures（见 server_fixture.h）。
//
// 覆盖（方案 §7.8）：
//   [N] board_graceful      —— 写板 → SIGTERM → 复起 → 值不变
//   [N] queue_graceful      —— 写队列 → SIGTERM → 复起 → 头部指针不变 + FIFO 读回
//   [B] kill9_crash         —— 写板+队列 → SIGKILL（无优雅 flush）→ 复起 → 仍一致（页缓存一致性）
//   [B] create_tag_then_crash —— 运行时建 tag+写值 → SIGKILL → 复起 → 新 tag 可读
//   [B] corrupt_file_startup  —— 把队列文件截断为 0 → 复起 → LoadQ 失败 → server 拒绝启动
//   [B] corrupt_exit_code    —— 同上，但进一步断言退出码==1（L1，已修复）
#include <unistd.h>  // truncate

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "framework/ts.h"
#include "cases/xfail_ids.h"

using namespace ts;

namespace {

constexpr int QREC = 64;  // Q_NORMAL_BIN 记录大小（= fx::Q_RECSIZE）
constexpr int QN   = 10;  // 写入记录数（< 容量 16）

// --- 已知板值快照：由种子派生，写入后任何重启都应逐字段不变 --------------------
struct BoardSnapshot {
    int32_t  i32  = 0;
    int64_t  i64  = 0;
    double   f64  = 0;
    uint32_t binSeq = 0;      // TAG_BIN256 负载的序列号（<2^31，便于 check_payload）
    std::vector<char> bin;    // TAG_BIN256：256B 自校验负载
    std::string str;          // STR_64：< 64 字节字符串
};

BoardSnapshot make_snapshot(uint64_t seed)
{
    Rng rng(seed);
    BoardSnapshot s;
    s.i32 = (int32_t)rng.next_u32();
    s.i64 = (int64_t)rng.next_u64();
    uint64_t bits = rng.next_u64();
    memcpy(&s.f64, &bits, sizeof(double));   // 任意位型 double（可能为 NaN → 用位比较）
    s.binSeq = rng.next_u32() & 0x7fffffffu;
    make_payload(s.bin, 256, s.binSeq, rng);
    char tmp[48];
    snprintf(tmp, sizeof(tmp), "persist-%08x", (unsigned)rng.next_u32());
    s.str = tmp;
    return s;
}

// 写入快照（用普通 writeb，即便触发投递也无人订阅）。
void write_snapshot(TestContext& ctx, int fd, const BoardSnapshot& s)
{
    unsigned err = 0;
    int32_t i32 = s.i32; int64_t i64 = s.i64; double f64 = s.f64;
    REQUIRE_OK(ctx, writeb(fd, fx::TAG_PERSIST, &i32, sizeof(i32), &err), err);
    REQUIRE_OK(ctx, writeb(fd, fx::TAG_I64, &i64, sizeof(i64), &err), err);
    REQUIRE_OK(ctx, writeb(fd, fx::TAG_F64, &f64, sizeof(f64), &err), err);
    REQUIRE_OK(ctx, writeb(fd, fx::TAG_BIN256, (void*)s.bin.data(), 256, &err), err);
    REQUIRE_OK(ctx, writeb_string(fd, fx::STR_64, s.str.c_str(), &err), err);
}

// 读回并逐字段比对（重启后调用）。
void verify_snapshot(TestContext& ctx, int fd, const BoardSnapshot& s)
{
    unsigned err = 0;
    int32_t i32 = 0; int64_t i64 = 0; double f64 = 0;
    EXPECT_OK(ctx, readb(fd, fx::TAG_PERSIST, &i32, sizeof(i32), &err, nullptr), err);
    CHECK(ctx, i32 == s.i32, "TAG_PERSIST %d != %d after restart", i32, s.i32);
    EXPECT_OK(ctx, readb(fd, fx::TAG_I64, &i64, sizeof(i64), &err, nullptr), err);
    CHECK(ctx, i64 == s.i64, "TAG_I64 mismatch after restart");
    EXPECT_OK(ctx, readb(fd, fx::TAG_F64, &f64, sizeof(f64), &err, nullptr), err);
    CHECK(ctx, memcmp(&f64, &s.f64, sizeof(double)) == 0, "TAG_F64 bit-mismatch after restart");
    std::vector<char> bin(256, 0);
    EXPECT_OK(ctx, readb(fd, fx::TAG_BIN256, bin.data(), 256, &err, nullptr), err);
    std::string perr;
    CHECK(ctx, check_payload(bin.data(), 256, (long)s.binSeq, &perr),
          "TAG_BIN256 after restart: %s", perr.c_str());
    char buf[128] = {0};
    EXPECT_OK(ctx, readb_string(fd, fx::STR_64, buf, sizeof(buf), &err, nullptr), err);
    CHECK(ctx, s.str == buf, "STR_64 '%s' != '%s' after restart", buf, s.str.c_str());
}

// 向 Q_NORMAL_BIN 写 n 条自校验记录（seq 0..n-1）。
void write_queue(TestContext& ctx, int fd, int n)
{
    Rng rng(ctx.seed ^ 0x5151515151515151ull);
    unsigned err = 0;
    for (int i = 0; i < n; i++) {
        std::vector<char> rec;
        make_payload(rec, QREC, (uint32_t)i, rng);
        REQUIRE_OK(ctx, writeq(fd, fx::Q_NORMAL_BIN, rec.data(), QREC, &err), err);
    }
}

// 读回 n 条并校验 FIFO 顺序 + 完整性（check_payload 用内嵌 crc 自证）。
void verify_queue(TestContext& ctx, int fd, int n)
{
    unsigned err = 0;
    for (int i = 0; i < n; i++) {
        std::vector<char> rec(QREC, 0);
        EXPECT_OK(ctx, readq(fd, fx::Q_NORMAL_BIN, rec.data(), QREC, &err), err);
        std::string perr;
        CHECK(ctx, check_payload(rec.data(), QREC, (long)i, &perr),
              "queue rec %d after restart: %s", i, perr.c_str());
    }
}

QUEUE_HEAD read_queue_head(TestContext& ctx, int fd)
{
    QUEUE_HEAD h{};
    unsigned err = 0;
    EXPECT_OK(ctx, readhead(fd, fx::Q_NORMAL_BIN, &h, &err), err);
    return h;
}

// 队列头应跨重启逐字段不变（未损坏 + 指针自洽的直接证据）。
void expect_head_eq(TestContext& ctx, const QUEUE_HEAD& a, const QUEUE_HEAD& b)
{
    CHECK(ctx,
          a.readPoint == b.readPoint && a.writePoint == b.writePoint && a.num == b.num &&
              a.size == b.size && a.dataType == b.dataType && a.operateMode == b.operateMode,
          "queue head changed across restart "
          "(rp %d/%d wp %d/%d num %d/%d size %d/%d type %d/%d mode %d/%d)",
          a.readPoint, b.readPoint, a.writePoint, b.writePoint, a.num, b.num, a.size, b.size,
          a.dataType, b.dataType, a.operateMode, b.operateMode);
}

// 在栈上构造并启动一个私有 fixture；失败即 ASSERT 终止本用例。
// 返回后 srv 已就绪；用例结束时其析构自动 stop() + 清理 sandbox。
#define PRIVATE_SERVER(SRV)                                                     \
    ServerConfig SRV##_cfg;                                                     \
    SRV##_cfg.threads = 2;                                                      \
    SRV##_cfg.asan = ctx.expectAsan;                                           \
    ServerFixture SRV(SRV##_cfg);                                              \
    ASSERT(ctx, SRV.start(), "private fixture start failed: %s", SRV.startup_error().c_str())

}  // namespace

// --- [N] BOARD 优雅重启：SIGTERM 后数据仍在 --------------------------------
TEST("persist.board_graceful", TAG_PERSIST)
{
    PRIVATE_SERVER(srv);
    BoardSnapshot snap = make_snapshot(ctx.seed);

    {
        ScopedConn c(srv);
        ASSERT(ctx, c.ok(), "connect before restart failed");
        write_snapshot(ctx, c.fd(), snap);
    }

    srv.stop();  // 优雅 SIGTERM → master 退出前 mmap 落盘
    ASSERT(ctx, srv.relaunch(), "relaunch failed: %s", srv.startup_error().c_str());

    {
        ScopedConn c(srv);
        ASSERT(ctx, c.ok(), "connect after restart failed");
        verify_snapshot(ctx, c.fd(), snap);
    }
    srv.stop();
}

// --- [N] QUEUE 优雅重启：头部指针不变 + FIFO 读回 --------------------------
TEST("persist.queue_graceful", TAG_PERSIST)
{
    PRIVATE_SERVER(srv);
    QUEUE_HEAD before{};

    {
        ScopedConn c(srv);
        ASSERT(ctx, c.ok(), "connect before restart failed");
        write_queue(ctx, c.fd(), QN);
        before = read_queue_head(ctx, c.fd());
        CHECK(ctx, before.writePoint != before.readPoint, "expected %d queued records pre-restart", QN);
    }

    srv.stop();
    ASSERT(ctx, srv.relaunch(), "relaunch failed: %s", srv.startup_error().c_str());

    {
        ScopedConn c(srv);
        ASSERT(ctx, c.ok(), "connect after restart failed");
        QUEUE_HEAD after = read_queue_head(ctx, c.fd());
        expect_head_eq(ctx, before, after);  // 指针/容量/类型跨重启不变
        verify_queue(ctx, c.fd(), QN);       // 内容按 FIFO 完整读回
        // 读空后读指针应追上写指针（队列确实被消费干净）。
        QUEUE_HEAD drained = read_queue_head(ctx, c.fd());
        CHECK(ctx, drained.readPoint == before.writePoint,
              "after draining readPoint %d != writePoint %d", drained.readPoint, before.writePoint);
    }
    srv.stop();
}

// --- [B] 崩溃一致性：SIGKILL（无优雅 flush）后数据仍在 ----------------------
// mmap(MAP_SHARED) 的脏页属于页缓存，进程被 SIGKILL 也不丢；复起后新 mmap 看到同样内容。
TEST("persist.kill9_crash", TAG_PERSIST)
{
    PRIVATE_SERVER(srv);
    BoardSnapshot snap = make_snapshot(ctx.seed);
    QUEUE_HEAD before{};

    {
        ScopedConn c(srv);
        ASSERT(ctx, c.ok(), "connect before kill failed");
        write_snapshot(ctx, c.fd(), snap);
        write_queue(ctx, c.fd(), QN);
        before = read_queue_head(ctx, c.fd());
    }

    srv.kill9();  // SIGKILL master+worker：无析构、无主动 msync
    ASSERT(ctx, srv.relaunch(), "relaunch after kill9 failed: %s", srv.startup_error().c_str());

    {
        ScopedConn c(srv);
        ASSERT(ctx, c.ok(), "connect after kill9 failed");
        verify_snapshot(ctx, c.fd(), snap);
        QUEUE_HEAD after = read_queue_head(ctx, c.fd());
        expect_head_eq(ctx, before, after);
        verify_queue(ctx, c.fd(), QN);
    }
    srv.stop();
}

// --- [B] 运行时建 tag 后崩溃 → 重启后该 tag 可读 ---------------------------
// 说明：网络 createtag 在 typesize==0 时会被拒绝（ERROR_PARAMETER_SIZE），故这里传一个
//   最小非空类型描述符（1 字节哑值）——CreateItem 只原样存储描述符、不解析，
//   tag 本身是 4 字节二进制项，writeb/readb 原始字节不受影响。
TEST("persist.create_tag_then_crash", TAG_PERSIST)
{
    PRIVATE_SERVER(srv);
    const char* NEWTAG = "T_CRASHTAG";
    int32_t val = (int32_t)Rng(ctx.seed ^ 0xABCDEF).next_u32();

    {
        ScopedConn c(srv);
        ASSERT(ctx, c.ok(), "connect before create failed");
        unsigned err = 0;
        char typeDummy = 0x01;  // typesize 必须 > 0
        EXPECT_OK(ctx, createtag(c.fd(), NEWTAG, (int)sizeof(int32_t), &typeDummy, 1, &err), err);
        REQUIRE_OK(ctx, writeb(c.fd(), NEWTAG, &val, sizeof(val), &err), err);
    }

    srv.kill9();
    ASSERT(ctx, srv.relaunch(), "relaunch after kill9 failed: %s", srv.startup_error().c_str());

    {
        ScopedConn c(srv);
        ASSERT(ctx, c.ok(), "connect after kill9 failed");
        unsigned err = 0;
        int32_t got = 0;
        EXPECT_OK(ctx, readb(c.fd(), NEWTAG, &got, sizeof(got), &err, nullptr), err);
        CHECK(ctx, got == val, "created tag value %d != %d after crash+restart", got, val);
    }
    srv.stop();
}

// --- [B] 启动遇损坏文件 → server 退出码 1 ----------------------------------
// LoadQ 对 0 字节文件执行 mmap(length=0) → EINVAL → 返回 false → gplat_load_qbd 失败 → 退出 1。
TEST("persist.corrupt_file_startup", TAG_PERSIST)
{
    PRIVATE_SERVER(srv);
    std::string qfile = srv.qbd_path() + "/" + fx::Q_NORMAL_BIN;  // <sandbox>/qbdfile/Q_NB
    srv.stop();  // 先停机，释放文件句柄

    // 截断为 0 字节（模拟损坏/半写）。
    ASSERT(ctx, truncate(qfile.c_str(), 0) == 0, "truncate %s: %s", qfile.c_str(), strerror(errno));

    bool up = srv.relaunch();
    CHECK(ctx, !up, "server should refuse to start with a corrupt (0-byte) qbd file");
    CHECK(ctx, !srv.running(), "server must not be running after corrupt-file load");
    printf("    [info] relaunch rejected as expected: %s\n", srv.startup_error().c_str());
    if (up) srv.stop();  // 万一意外起来，收尾
}

// --- [B][L1 已修复] 致命启动失败应以退出码 1 退出 ----------------------------
// 背景（nginx.cxx）：gplat_load_qbd() 失败时置 exitcode=1; goto lblexit; return exitcode。
//   进程管理器（systemd/supervisor）依赖非 0 退出码识别启动失败，故作为常规门控回归保护。
// 断言：损坏文件 → 拒绝启动且退出码==1。
TEST("persist.corrupt_exit_code", TAG_PERSIST)
{
    PRIVATE_SERVER(srv);
    std::string qfile = srv.qbd_path() + "/" + fx::Q_NORMAL_BIN;
    srv.stop();  // 先停机释放文件句柄

    ASSERT(ctx, truncate(qfile.c_str(), 0) == 0, "truncate %s: %s", qfile.c_str(), strerror(errno));

    bool up = srv.relaunch();
    ASSERT(ctx, !up, "server should refuse to start with a corrupt (0-byte) qbd file");
    CHECK(ctx, !srv.running(), "server must not be running after corrupt-file load");
    // 核心断言：致命启动失败必须以非 0（约定为 1）退出码告知上层进程管理器。
    CHECK(ctx, srv.last_exit_code() == 1,
          "fatal startup failure should exit with code 1, got %d (L1 regression)", srv.last_exit_code());
    if (up) srv.stop();
}
