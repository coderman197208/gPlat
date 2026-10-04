// test_queue.cpp —— QUEUE 基础与边界用例（方案 §7.4）
//
// 覆盖 API：writeq / readq / clearq / peekq / readhead / listq（createqueue 见末尾 [B]）。
// 夹具队列（fixtures.cpp 用本地 API 预建，含 CLI 不支持的 ASCII 队列）：
//   Q_NB NORMAL/BINARY, Q_SB SHIFT/BINARY, Q_NA NORMAL/ASCII, Q_SA SHIFT/ASCII（recsize 64, num 17）
//   Q_BIG NORMAL/BINARY（recsize 2048, num 5）
//
// 共享 server 纪律：每个用例入口 clearq 自己要用的队列，确保与前序用例隔离（Runner 顺序执行）。
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "framework/ts.h"
#include "cases/xfail_ids.h"

using namespace ts;

namespace {

// 构造一条 size 字节、带 seq 自校验头的 BINARY 记录。
void rec_make(std::vector<char>& buf, int size, uint32_t seq, Rng& rng)
{
    make_payload(buf, size, seq, rng);
}

}  // namespace

// --- [N] NORMAL/BINARY：FIFO 顺序、容量 num-1、满→DQ_FULL、空→DQ_EMPTY ------
TEST("queue.normal_fifo_capacity", TAG_QUEUE)
{
    ScopedConn c(ctx.server());
    ASSERT(ctx, c.ok(), "connectgplat failed");
    int fd = c.fd();
    unsigned err = 0;
    Rng rng(ctx.seed);
    const int SZ = fx::Q_RECSIZE;  // 64

    REQUIRE_OK(ctx, clearq(fd, fx::Q_NORMAL_BIN, &err), err);

    // 写到满：自适应统计容量，断言 == num-1（不写死，自证实现语义）。
    int accepted = 0;
    std::vector<char> buf;
    for (int i = 0; i < fx::Q_COUNT + 5; i++) {
        rec_make(buf, SZ, (uint32_t)i, rng);
        err = 0;
        if (writeq(fd, fx::Q_NORMAL_BIN, buf.data(), SZ, &err)) { accepted++; continue; }
        CHECK(ctx, err == ERROR_DQ_FULL, "write past capacity: want DQ_FULL got %u(%s)",
              err, err_name(err));
        break;
    }
    CHECK(ctx, accepted == fx::Q_COUNT - 1, "NORMAL capacity: want num-1=%d accepted=%d",
          fx::Q_COUNT - 1, accepted);

    // 按 FIFO 读回，校验顺序（seq 0,1,2,...）。
    for (int i = 0; i < accepted; i++) {
        std::vector<char> rd(SZ, 0);
        REQUIRE_OK(ctx, readq(fd, fx::Q_NORMAL_BIN, rd.data(), SZ, &err), err);
        std::string perr;
        CHECK(ctx, check_payload(rd.data(), SZ, i, &perr), "FIFO pos %d: %s", i, perr.c_str());
    }
    // 读空 → DQ_EMPTY。
    std::vector<char> rd(SZ, 0);
    EXPECT_ERR(ctx, readq(fd, fx::Q_NORMAL_BIN, rd.data(), SZ, &err), err, ERROR_DQ_EMPTY);
}

// --- [N] SHIFT/BINARY：写满不溢出（覆盖最旧）；readq 行为自证 ---------------
TEST("queue.shift_overwrite", TAG_QUEUE)
{
    ScopedConn c(ctx.server());
    ASSERT(ctx, c.ok(), "connectgplat failed");
    int fd = c.fd();
    unsigned err = 0;
    Rng rng(ctx.seed);
    const int SZ = fx::Q_RECSIZE;

    REQUIRE_OK(ctx, clearq(fd, fx::Q_SHIFT_BIN, &err), err);

    // 写入远多于容量的记录：SHIFT 不应返回 DQ_FULL（覆盖最旧）。
    const int TOTAL = fx::Q_COUNT * 2;  // 34 > 容量
    std::vector<char> buf;
    for (int i = 0; i < TOTAL; i++) {
        rec_make(buf, SZ, (uint32_t)i, rng);
        err = 0;
        bool ok = writeq(fd, fx::Q_SHIFT_BIN, buf.data(), SZ, &err);
        CHECK(ctx, ok && err == 0, "SHIFT write %d should not overflow: ret=%d err=%u(%s)",
              i, (int)ok, err, err_name(err));
    }

    // 读回若干条，记录实际返回的 seq（SHIFT 读语义以实现为准，用于自证/回归锚点）。
    std::vector<char> rd(SZ, 0);
    err = 0;
    if (readq(fd, fx::Q_SHIFT_BIN, rd.data(), SZ, &err)) {
        std::string perr;
        bool intact = check_payload(rd.data(), SZ, -1, &perr);  // 不校验 seq，仅校验完整性
        CHECK(ctx, intact, "SHIFT readq record corrupt: %s", perr.c_str());
        const PayloadTag* pt = reinterpret_cast<const PayloadTag*>(rd.data());
        printf("    [info] SHIFT first readq -> seq=%u (total written %d)\n", pt->seq, TOTAL);
    } else {
        printf("    [info] SHIFT readq after fill -> err=%u(%s)\n", err, err_name(err));
    }
}

// --- [B] BINARY actsize != 记录大小 → RECORDSIZE ---------------------------
TEST("queue.binary_recordsize_mismatch", TAG_QUEUE)
{
    ScopedConn c(ctx.server());
    ASSERT(ctx, c.ok(), "connectgplat failed");
    int fd = c.fd();
    unsigned err = 0;
    REQUIRE_OK(ctx, clearq(fd, fx::Q_NORMAL_BIN, &err), err);

    std::vector<char> rec(256, 0x33);
    // 写入 actsize=32（<64）→ RECORDSIZE
    EXPECT_ERR(ctx, writeq(fd, fx::Q_NORMAL_BIN, rec.data(), 32, &err), err, ERROR_RECORDSIZE);
    // 写入 actsize=128（>64）→ RECORDSIZE
    EXPECT_ERR(ctx, writeq(fd, fx::Q_NORMAL_BIN, rec.data(), 128, &err), err, ERROR_RECORDSIZE);
}

// --- [B] peekq：空队列→DQ_EMPTY；PEEK_NEXT/LATEST 语义；peek 不消费 ---------
TEST("queue.peek_semantics", TAG_QUEUE)
{
    ScopedConn c(ctx.server());
    ASSERT(ctx, c.ok(), "connectgplat failed");
    int fd = c.fd();
    unsigned err = 0;
    Rng rng(ctx.seed);
    const int SZ = fx::Q_RECSIZE;

    REQUIRE_OK(ctx, clearq(fd, fx::Q_NORMAL_BIN, &err), err);

    // 空队列 peek → DQ_EMPTY
    std::vector<char> rd(SZ, 0);
    RECORD_HEAD rh{};
    EXPECT_ERR(ctx, peekq(fd, fx::Q_NORMAL_BIN, PEEK_NEXT, rd.data(), SZ, &rh, &err),
               err, ERROR_DQ_EMPTY);

    // 写 r0, r1
    std::vector<char> buf;
    rec_make(buf, SZ, 0, rng); REQUIRE_OK(ctx, writeq(fd, fx::Q_NORMAL_BIN, buf.data(), SZ, &err), err);
    rec_make(buf, SZ, 1, rng); REQUIRE_OK(ctx, writeq(fd, fx::Q_NORMAL_BIN, buf.data(), SZ, &err), err);

    // PEEK_NEXT 应看到 r0（下一次 readq 将返回者），且不消费
    std::fill(rd.begin(), rd.end(), 0);
    REQUIRE_OK(ctx, peekq(fd, fx::Q_NORMAL_BIN, PEEK_NEXT, rd.data(), SZ, &rh, &err), err);
    {
        std::string perr;
        CHECK(ctx, check_payload(rd.data(), SZ, 0, &perr), "PEEK_NEXT want seq0: %s", perr.c_str());
    }
    // PEEK_LATEST 应看到 r1（最近写入）
    std::fill(rd.begin(), rd.end(), 0);
    REQUIRE_OK(ctx, peekq(fd, fx::Q_NORMAL_BIN, PEEK_LATEST, rd.data(), SZ, &rh, &err), err);
    {
        std::string perr;
        CHECK(ctx, check_payload(rd.data(), SZ, 1, &perr), "PEEK_LATEST want seq1: %s", perr.c_str());
    }
    // 真正 readq 仍应拿到 r0（证明 peek 未移动读指针）
    std::fill(rd.begin(), rd.end(), 0);
    REQUIRE_OK(ctx, readq(fd, fx::Q_NORMAL_BIN, rd.data(), SZ, &err), err);
    {
        std::string perr;
        CHECK(ctx, check_payload(rd.data(), SZ, 0, &perr), "readq after peek want seq0: %s", perr.c_str());
    }
}

// --- [N] clearq：写入后清空 → 读空 ----------------------------------------
TEST("queue.clear", TAG_QUEUE)
{
    ScopedConn c(ctx.server());
    ASSERT(ctx, c.ok(), "connectgplat failed");
    int fd = c.fd();
    unsigned err = 0;
    Rng rng(ctx.seed);
    const int SZ = fx::Q_RECSIZE;

    std::vector<char> buf;
    for (int i = 0; i < 5; i++) {
        rec_make(buf, SZ, (uint32_t)i, rng);
        REQUIRE_OK(ctx, writeq(fd, fx::Q_NORMAL_BIN, buf.data(), SZ, &err), err);
    }
    REQUIRE_OK(ctx, clearq(fd, fx::Q_NORMAL_BIN, &err), err);
    std::vector<char> rd(SZ, 0);
    EXPECT_ERR(ctx, readq(fd, fx::Q_NORMAL_BIN, rd.data(), SZ, &err), err, ERROR_DQ_EMPTY);
}

// --- [N] readhead：读队列头，字段与建队一致 --------------------------------
TEST("queue.readhead_fields", TAG_QUEUE)
{
    ScopedConn c(ctx.server());
    ASSERT(ctx, c.ok(), "connectgplat failed");
    int fd = c.fd();
    unsigned err = 0;
    REQUIRE_OK(ctx, clearq(fd, fx::Q_NORMAL_BIN, &err), err);

    QUEUE_HEAD h{};
    REQUIRE_OK(ctx, readhead(fd, fx::Q_NORMAL_BIN, &h, &err), err);
    CHECK(ctx, h.size == fx::Q_RECSIZE, "head.size want %d got %d", fx::Q_RECSIZE, h.size);
    CHECK(ctx, h.num == fx::Q_COUNT, "head.num want %d got %d", fx::Q_COUNT, h.num);
    CHECK(ctx, h.dataType == BINARY_TYPE, "head.dataType want BINARY got %d", h.dataType);
    CHECK(ctx, h.operateMode == NORMAL_MODE, "head.operateMode want NORMAL got %d", h.operateMode);
}

// --- [N] listq：已加载队列名包含我们的夹具队列 -----------------------------
TEST("queue.listq_contains_fixtures", TAG_QUEUE)
{
    ScopedConn c(ctx.server());
    ASSERT(ctx, c.ok(), "connectgplat failed");
    int fd = c.fd();
    unsigned err = 0;

    std::vector<char> names(8192, 0);
    int count = 0;
    REQUIRE_OK(ctx, listq(fd, names.data(), (int)names.size(), &count, &err), err);
    // names 是以 '\0' 分隔的名字序列；在其中查找 Q_NB。
    auto contains = [&](const char* want) -> bool {
        const char* p = names.data();
        const char* end = names.data() + names.size();
        for (int i = 0; i < count && p < end; i++) {
            if (strcmp(p, want) == 0) return true;
            p += strlen(p) + 1;
        }
        return false;
    };
    CHECK(ctx, count > 0, "listq returned count=0");
    CHECK(ctx, contains(fx::Q_NORMAL_BIN), "listq missing %s (count=%d)", fx::Q_NORMAL_BIN, count);
}

// --- [B] 大记录队列 Q_BIG（recsize 2048）写读一致 --------------------------
TEST("queue.big_record_roundtrip", TAG_QUEUE)
{
    ScopedConn c(ctx.server());
    ASSERT(ctx, c.ok(), "connectgplat failed");
    int fd = c.fd();
    unsigned err = 0;
    Rng rng(ctx.seed);
    const int SZ = fx::Q_BIG_RECSIZE;  // 2048

    REQUIRE_OK(ctx, clearq(fd, fx::Q_BIG_BIN, &err), err);
    std::vector<char> buf;
    rec_make(buf, SZ, 42, rng);
    REQUIRE_OK(ctx, writeq(fd, fx::Q_BIG_BIN, buf.data(), SZ, &err), err);
    std::vector<char> rd(SZ, 0);
    REQUIRE_OK(ctx, readq(fd, fx::Q_BIG_BIN, rd.data(), SZ, &err), err);
    std::string perr;
    CHECK(ctx, check_payload(rd.data(), SZ, 42, &perr), "big record: %s", perr.c_str());
}

// --- [I] clearq(error==NULL)：安全返回 false，不崩溃 ---------------------------
// （原缺陷 C5 已修复：clearq 以前在 error==NULL 时仍执行 `*error = ...`。）fork 隔离，防止回归时拖垮套件。
TEST("queue.clearq_null_error", TAG_QUEUE | TAG_DESTRUCTIVE)
{
    ServerConfig cfg;
    cfg.threads = 2;
    cfg.asan = ctx.expectAsan;
    ServerFixture srv(cfg);
    ASSERT(ctx, srv.start(), "private fixture start failed: %s", srv.startup_error().c_str());
    const std::string ip = srv.ip();
    const int port = srv.port();

    ForkResult r = run_in_fork([&]() {
        int fd = connectgplat(ip.c_str(), port);
        if (fd < 0) _exit(77);
        bool ok = clearq(fd, fx::Q_NORMAL_BIN, nullptr);
        _exit(ok ? 2 : 0);
    }, 4000);

    ASSERT(ctx, !(r.exited && r.exit_code == 77), "C5 child could not connect");
    CHECK(ctx, r.exited && r.exit_code == 0,
          "clearq(error=NULL) not handled safely —— %s", r.describe());
}
