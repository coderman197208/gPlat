// test_error.cpp —— 客户端错误上报：错误类别 / SetErrorHook / 默认 stderr 输出
//
// 覆盖 API：GetErrorCategory / IsFatalError / SetErrorHook，以及网络 API 返回时的错误上报（GPLAT_ERRCAT_*）。
//   [N] category_table               —— 代表性错误码的类别：业务结果 RESULT、编程错误 USAGE、
//                                        连接不可用 CONNECTION（errno 带 strerror 描述）；IsFatalError 等价于 USAGE。
//   [N] hook_reports_category_and_func —— 钩子收到错误码、类别、描述和 API 名；经内部 helper 实现的
//                                        writeb / writeb_string / write_plc_int 每次调用只上报一次
//                                        （服务端对 PLC 写总是回成功，write_plc_int 用失效 fd 触发失败）；
//                                        成功调用不上报；SetErrorHook(NULL) 后不再上报。
//   [N] default_hook_stderr          —— 默认钩子只把 USAGE / CONNECTION 类错误各写一行到 stderr，
//                                        RESULT 类不输出，SetErrorHook(NULL) 后静默。
//
// 钩子是进程级全局状态：改钩子的用例都在 fork 子进程里跑，不影响其它用例。
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "framework/ts.h"

using namespace ts;

namespace {

struct HookCall {
    unsigned error;
    int category;
    std::string message;
    std::string func;
};

void record_hook(unsigned int error, int category, const char* message, const char* func, void* user)
{
    static_cast<std::vector<HookCall>*>(user)->push_back(
        {error, category, message ? message : "", func ? func : ""});
}

constexpr const char* MISSING_TAG = "NO_SUCH_TAG_errhook";

}  // namespace

// --- [N] 错误类别表：RESULT 只返回码，USAGE / CONNECTION 由 GplatConnection 抛异常 ---------
TEST("error.category_table", TAG_BOARD)
{
    struct { unsigned code; int category; } cases[] = {
        {0, GPLAT_ERRCAT_RESULT},
        {ERROR_TAG_NOT_EXIST, GPLAT_ERRCAT_RESULT},
        {ERROR_DQ_EMPTY, GPLAT_ERRCAT_RESULT},
        {ERROR_DQ_FULL, GPLAT_ERRCAT_RESULT},
        {ERROR_ITEM_ALREADY_EXIST, GPLAT_ERRCAT_RESULT},
        {ERROR_WAIT_TIMEOUT, GPLAT_ERRCAT_RESULT},
        {ERROR_RESPONSE_TIMEOUT, GPLAT_ERRCAT_RESULT},
        {ERROR_REQUEST_QUEUE_FULL, GPLAT_ERRCAT_RESULT},
        {ERROR_INVALID_PARAMETER, GPLAT_ERRCAT_USAGE},
        {ERROR_PARAMETER_SIZE, GPLAT_ERRCAT_USAGE},
        {ERROR_RECORDSIZE, GPLAT_ERRCAT_USAGE},
        {STRING_TOO_LONG, GPLAT_ERRCAT_USAGE},
        {BUFFER_TOO_SMALL, GPLAT_ERRCAT_USAGE},
        {ERROR_BUFFER_TOO_SMALL, GPLAT_ERRCAT_USAGE},
        {ECONNRESET, GPLAT_ERRCAT_CONNECTION},
        {EPIPE, GPLAT_ERRCAT_CONNECTION},
        {ERROR_SOCKET_NOT_CONNECTED, GPLAT_ERRCAT_CONNECTION},
        {ERROR_INVALID_RESPONSE, GPLAT_ERRCAT_CONNECTION},
    };
    for (const auto& c : cases) {
        const char* message = nullptr;
        int category = GetErrorCategory(c.code, &message);
        CHECK(ctx, category == c.category, "code %u: category %d, expected %d", c.code, category, c.category);
        CHECK(ctx, message != nullptr && message[0] != '\0', "code %u: empty message", c.code);
        CHECK(ctx, IsFatalError(c.code, nullptr) == (c.category == GPLAT_ERRCAT_USAGE),
              "code %u: IsFatalError must equal category == USAGE", c.code);
    }

    const char* message = nullptr;
    GetErrorCategory(ECONNRESET, &message);
    CHECK(ctx, message != nullptr && strcmp(message, strerror(ECONNRESET)) == 0,
          "errno message '%s', expected strerror", message ? message : "(null)");
}

// --- [N] 钩子收到的错误码 / 类别 / 描述 / API 名；每次 API 调用只上报一次 -----------
TEST("error.hook_reports_category_and_func", TAG_BOARD)
{
    const std::string ip = ctx.server().ip();
    const int port = ctx.server().port();

    ForkResult r = run_in_fork([&]() {
        std::vector<HookCall> calls;
        SetErrorHook(record_hook, &calls);
        int fd = connectgplat(ip.c_str(), port);
        if (fd < 0) _exit(77);
        unsigned err = 0;
        int32_t v = 1;
        char buf[16] = {0};

        // RESULT：读不存在的 tag
        readb(fd, MISSING_TAG, buf, sizeof(buf), &err, nullptr);
        if (calls.size() != 1 || calls[0].error != ERROR_TAG_NOT_EXIST ||
            calls[0].category != GPLAT_ERRCAT_RESULT || calls[0].message != "tag not exist" ||
            calls[0].func != "readb") {
            fprintf(stderr, "readb missing tag: %zu calls\n", calls.size());
            _exit(2);
        }

        // 经内部 helper（writeb_ / writeb_string_ / writeb_plc）实现的 API：只上报一次，func 为对外名
        size_t expected = 1;
        auto expect_once = [&](const char* func, bool ok) {
            expected++;
            if (ok || calls.size() != expected || calls.back().func != func) {
                fprintf(stderr, "%s: ok=%d, %zu calls (expected %zu), last func '%s'\n", func, ok,
                        calls.size(), expected, calls.back().func.c_str());
                _exit(3);
            }
        };
        int64_t big = 0;
        expect_once("writeb", writeb(fd, fx::TAG_I32, &big, sizeof(big), &err));   // USAGE：大小不符
        if (calls.back().error != ERROR_RECORDSIZE || calls.back().category != GPLAT_ERRCAT_USAGE) {
            fprintf(stderr, "writeb wrong size: err %u category %d\n", calls.back().error, calls.back().category);
            _exit(3);
        }
        expect_once("writeb_string", writeb_string(fd, MISSING_TAG, "x", &err));
        // 服务端对 PLC 写总是回成功，用失效 fd 触发 writeb_plc 的失败路径
        expect_once("write_plc_int", write_plc_int(-1, MISSING_TAG, 1, &err));

        // 成功调用不上报
        if (!readb(fd, fx::TAG_I32, &v, sizeof(v), &err, nullptr) || calls.size() != expected) {
            fprintf(stderr, "successful readb: err %u, %zu calls\n", err, calls.size());
            _exit(4);
        }

        // CONNECTION：失效 fd 上 send 失败，错误码为 errno，描述为 strerror
        readb(-1, fx::TAG_I32, &v, sizeof(v), &err, nullptr);
        if (calls.size() != expected + 1 || calls.back().error != EBADF ||
            calls.back().category != GPLAT_ERRCAT_CONNECTION ||
            calls.back().message != strerror(EBADF)) {
            fprintf(stderr, "readb bad fd: err %u, %zu calls\n", err, calls.size());
            _exit(5);
        }

        // NULL 关闭上报
        SetErrorHook(nullptr, nullptr);
        readb(fd, MISSING_TAG, buf, sizeof(buf), &err, nullptr);
        if (err != ERROR_TAG_NOT_EXIST || calls.size() != expected + 1) _exit(6);

        disconnectgplat(fd);
        _exit(0);
    }, 4000);

    ASSERT(ctx, !(r.exited && r.exit_code == 77), "child could not connect");
    CHECK(ctx, r.exited && r.exit_code == 0, "error hook mismatch (%s): %s", r.describe(),
          r.diagnostics.c_str());
}

// --- [N] 默认钩子：USAGE / CONNECTION 各一行写 stderr，RESULT 不输出，NULL 静默 ------
TEST("error.default_hook_stderr", TAG_BOARD)
{
    const std::string ip = ctx.server().ip();
    const int port = ctx.server().port();

    ForkResult r = run_in_fork([&]() {
        int fd = connectgplat(ip.c_str(), port);
        if (fd < 0) _exit(77);
        unsigned err = 0;
        int32_t v = 0;
        int64_t big = 0;
        char buf[16] = {0};

        readb(fd, MISSING_TAG, buf, sizeof(buf), &err, nullptr);    // RESULT：不输出
        writeb(fd, fx::TAG_I32, &big, sizeof(big), &err);           // USAGE（服务端）
        readb(fd, nullptr, &v, sizeof(v), &err, nullptr);           // USAGE（客户端参数校验）
        readb(-1, fx::TAG_I32, &v, sizeof(v), &err, nullptr);       // CONNECTION (EBADF)

        SetErrorHook(nullptr, nullptr);
        writeb(fd, fx::TAG_I32, &big, sizeof(big), &err);
        readb(-1, fx::TAG_I32, &v, sizeof(v), &err, nullptr);

        disconnectgplat(fd);
        _exit(0);
    }, 4000);

    ASSERT(ctx, !(r.exited && r.exit_code == 77), "child could not connect");
    ASSERT(ctx, r.exited && r.exit_code == 0, "child failed (%s)", r.describe());

    char expected[512];
    snprintf(expected, sizeof(expected),
             "[higplat usage] writeb: record size invalid (code %u)\n"
             "[higplat usage] readb: invalid parameter (code %u)\n"
             "[higplat connection] readb: %s (code %d)\n",
             (unsigned)ERROR_RECORDSIZE, (unsigned)ERROR_INVALID_PARAMETER, strerror(EBADF), EBADF);
    CHECK(ctx, r.diagnostics == expected, "stderr mismatch:\n--- expected\n%s--- got\n%s", expected,
          r.diagnostics.c_str());
}
