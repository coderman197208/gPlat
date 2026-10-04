// test_string.cpp —— 字符串 BOARD 与 String 理论上限（方案 §7.3 / §7.5）
//
// 覆盖 API：writeb_string / writeb_string_notpost / readb_string。
// 字符串 tag（fx::）：S_CAP1(1) S_CAP16(16) S_CAP64(64) S_CAPMAX(16384)。
// 注意：writeb_string 无显式长度参数，按 strlen(value) 发送；readb_string 读进 buffersize 缓冲。
//   value==NULL 会在客户端 strlen(NULL) 崩溃（原缺陷 C4，已修复），属破坏性，放 test_protocol，不在此文件。
//
// 共享 server 纪律：这些 tag 专供字符串用例；用例间顺序执行、各自覆盖写，互不影响。
#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

#include "framework/ts.h"
#include "cases/xfail_ids.h"

using namespace ts;

namespace {

// 生成长度 n 的可打印字符串（无嵌入 \0），内容由 seq 决定，便于核对。
std::string gen_str(int n, int seq)
{
    std::string s;
    s.reserve(n);
    for (int i = 0; i < n; i++) s.push_back((char)('A' + ((i + seq) % 26)));
    return s;
}

}  // namespace

// --- [N] 变长字符串写读一致 -----------------------------------------------
TEST("string.roundtrip", TAG_STRING)
{
    ScopedConn c(ctx.server());
    ASSERT(ctx, c.ok(), "connectgplat failed");
    int fd = c.fd();
    unsigned err = 0;

    std::string msg = gen_str(40, 3);  // 40 < cap64
    REQUIRE_OK(ctx, writeb_string(fd, fx::STR_64, msg.c_str(), &err), err);

    char buf[128] = {0};
    REQUIRE_OK(ctx, readb_string(fd, fx::STR_64, buf, sizeof(buf), &err, nullptr), err);
    CHECK(ctx, strcmp(buf, msg.c_str()) == 0, "roundtrip mismatch: wrote '%s' read '%s'",
          msg.c_str(), buf);
}

// --- [N] writeb_string_notpost：不触发订阅，值照样可读回 -------------------
TEST("string.notpost_roundtrip", TAG_STRING)
{
    ScopedConn c(ctx.server());
    ASSERT(ctx, c.ok(), "connectgplat failed");
    int fd = c.fd();
    unsigned err = 0;

    std::string msg = gen_str(20, 9);
    REQUIRE_OK(ctx, writeb_string_notpost(fd, fx::STR_64, msg.c_str(), &err), err);
    char buf[128] = {0};
    REQUIRE_OK(ctx, readb_string(fd, fx::STR_64, buf, sizeof(buf), &err, nullptr), err);
    CHECK(ctx, strcmp(buf, msg.c_str()) == 0, "notpost mismatch: wrote '%s' read '%s'",
          msg.c_str(), buf);
}

// --- [B] 空字符串（len 0）写读 --------------------------------------------
TEST("string.empty", TAG_STRING)
{
    ScopedConn c(ctx.server());
    ASSERT(ctx, c.ok(), "connectgplat failed");
    int fd = c.fd();
    unsigned err = 0;

    REQUIRE_OK(ctx, writeb_string(fd, fx::STR_64, "", &err), err);
    char buf[64];
    memset(buf, 'x', sizeof(buf));
    REQUIRE_OK(ctx, readb_string(fd, fx::STR_64, buf, sizeof(buf), &err, nullptr), err);
    CHECK(ctx, buf[0] == '\0', "empty string read: buf[0]=0x%02x (want NUL)", (unsigned char)buf[0]);
}

// --- [B] 恰满容量（无 \0 余量）：记录 readb_string 结尾截断语义 ------------
TEST("string.exact_capacity", TAG_STRING)
{
    ScopedConn c(ctx.server());
    ASSERT(ctx, c.ok(), "connectgplat failed");
    int fd = c.fd();
    unsigned err = 0;

    // 向 cap16 tag 写 16 字符（strlen=16=itemsize，无 \0 余量）
    std::string msg = gen_str(16, 1);
    err = 0;
    bool wok = writeb_string(fd, fx::STR_16, msg.c_str(), &err);
    printf("    [info] write 16 chars into cap16: ret=%d err=%u(%s)\n",
           (int)wok, err, err_name(err));
    if (wok && err == 0) {
        char buf[64] = {0};
        REQUIRE_OK(ctx, readb_string(fd, fx::STR_16, buf, sizeof(buf), &err, nullptr), err);
        // 记录读回长度：实现可能把第 16 字节用作 '\0' 而截断到 15 字符。
        printf("    [info] read back strlen=%zu (wrote 16)\n", strlen(buf));
        CHECK(ctx, strlen(buf) == 16 || strlen(buf) == 15,
              "exact-capacity readback strlen=%zu (expect 15 or 16)", strlen(buf));
    }
}

// --- [B] 超容量字符串 → STRING_TOO_LONG -----------------------------------
TEST("string.over_capacity", TAG_STRING)
{
    ScopedConn c(ctx.server());
    ASSERT(ctx, c.ok(), "connectgplat failed");
    int fd = c.fd();
    unsigned err = 0;

    std::string msg = gen_str(17, 2);  // 17 > cap16
    EXPECT_ERR(ctx, writeb_string(fd, fx::STR_16, msg.c_str(), &err), err, STRING_TOO_LONG);
}

// --- [B] readb_string 缓冲过小：记录语义（截断 or BUFFER_TOO_SMALL） --------
TEST("string.read_small_buffer", TAG_STRING)
{
    ScopedConn c(ctx.server());
    ASSERT(ctx, c.ok(), "connectgplat failed");
    int fd = c.fd();
    unsigned err = 0;

    std::string msg = gen_str(40, 5);
    REQUIRE_OK(ctx, writeb_string(fd, fx::STR_64, msg.c_str(), &err), err);

    char small[8] = {0};
    err = 0;
    bool ok = readb_string(fd, fx::STR_64, small, sizeof(small), &err, nullptr);
    printf("    [info] read 40-char string into 8-byte buf: ret=%d err=%u(%s)\n",
           (int)ok, err, err_name(err));
    // 不强判定具体码（实现相关）：要么成功且安全截断，要么报缓冲过小；关键是不得越界/崩溃。
    CHECK(ctx, ok || err != 0, "small-buffer read: neither success nor error code set");
}

// --- [B][§7.5] String 理论上限：逐步逼近 MAXMSGLEN=16384 -------------------
TEST("string.upper_limit", TAG_STRING)
{
    ScopedConn c(ctx.server());
    ASSERT(ctx, c.ok(), "connectgplat failed");
    int fd = c.fd();
    unsigned err = 0;

    // 16383 → 应成功（< cap16384 且 < MAXMSGLEN）
    {
        std::string msg = gen_str(16383, 0);
        REQUIRE_OK(ctx, writeb_string(fd, fx::STR_MAX, msg.c_str(), &err), err);
        std::vector<char> buf(16384 + 16, 0);
        REQUIRE_OK(ctx, readb_string(fd, fx::STR_MAX, buf.data(), (int)buf.size(), &err, nullptr), err);
        CHECK(ctx, strcmp(buf.data(), msg.c_str()) == 0, "16383 roundtrip mismatch");
    }
    // 16384 → 恰满 MAXMSGLEN 与 cap：记录是否接受（无 \0 余量，边界）
    {
        std::string msg = gen_str(16384, 0);
        err = 0;
        bool ok = writeb_string(fd, fx::STR_MAX, msg.c_str(), &err);
        printf("    [info] write 16384-char string: ret=%d err=%u(%s)\n",
               (int)ok, err, err_name(err));
        CHECK(ctx, ok || err != 0, "16384 write: neither success nor error set");
    }
    // 16385 → strlen>MAXMSGLEN → 客户端 PARAMETER_SIZE（不应发出）
    {
        std::string msg = gen_str(16385, 0);
        EXPECT_ERR(ctx, writeb_string(fd, fx::STR_MAX, msg.c_str(), &err), err, ERROR_PARAMETER_SIZE);
    }
}

// --- [I] writeb_string(value==NULL)：干净拒绝（false + INVALID_PARAMETER），不崩溃 ---
// （原缺陷 C4 已修复：writeb_string_ 以前在判空之前就 strlen(value)。）fork 隔离，防止回归时拖垮套件。
TEST("string.writeb_string_null_value", TAG_STRING | TAG_DESTRUCTIVE)
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
        unsigned err = 0;
        bool ok = writeb_string(fd, fx::STR_64, nullptr, &err);
        _exit((!ok && err == ERROR_INVALID_PARAMETER) ? 0 : 2);
    }, 4000);

    ASSERT(ctx, !(r.exited && r.exit_code == 77), "C4 child could not connect");
    CHECK(ctx, r.exited && r.exit_code == 0,
          "writeb_string(NULL) not rejected cleanly —— %s", r.describe());
}
