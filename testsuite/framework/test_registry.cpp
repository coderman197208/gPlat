// test_registry.cpp —— 见 test_registry.h
#include "test_registry.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <stdexcept>
#include <string>
#include <unistd.h>  // getpid

#include "higplat.h"        // ERROR_* 码
#include "server_fixture.h"

namespace ts {

static thread_local TestContext* activeContext = nullptr;
TestContext* current_context() { return activeContext; }

// ---------------------------------------------------------------------------
// 全局注册表
// ---------------------------------------------------------------------------
static std::vector<TestCase>& registry()
{
    static std::vector<TestCase> v;
    return v;
}
void register_case(const TestCase& tc) { registry().push_back(tc); }
const std::vector<TestCase>& all_cases() { return registry(); }

// ---------------------------------------------------------------------------
// 标签名字映射
// ---------------------------------------------------------------------------
struct TagName { uint32_t bit; const char* name; };
static const TagName kTagNames[] = {
    {TAG_BOARD, "board"},     {TAG_QUEUE, "queue"},       {TAG_STRING, "string"},
    {TAG_PUBSUB, "pubsub"},   {TAG_GETRESP, "getresp"},   {TAG_PERSIST, "persist"},
    {TAG_MEMORY, "memory"},   {TAG_CHURN, "churn"},       {TAG_CONN, "conn"},
    {TAG_STRESS, "stress"},   {TAG_PROTOCOL, "protocol"}, {TAG_DESTRUCTIVE, "destructive"},
    {TAG_ASAN, "asan"},       {TAG_WHITEBOX, "whitebox"},
};

uint32_t tag_from_name(const std::string& name)
{
    for (const auto& t : kTagNames)
        if (name == t.name) return t.bit;
    return 0;
}

std::string tags_to_string(uint32_t tags)
{
    std::string s;
    for (const auto& t : kTagNames)
        if (tags & t.bit) { if (!s.empty()) s += ","; s += t.name; }
    return s;
}

// ---------------------------------------------------------------------------
// 错误码名字
// ---------------------------------------------------------------------------
struct ErrName { unsigned code; const char* name; };
static const ErrName kErrNames[] = {
    {0, "OK"},
    {ERROR_DQFILE_NOT_FOUND, "DQFILE_NOT_FOUND"}, {ERROR_DQ_NOT_OPEN, "DQ_NOT_OPEN"},
    {ERROR_DQ_EMPTY, "DQ_EMPTY"}, {ERROR_DQ_FULL, "DQ_FULL"},
    {ERROR_FILENAME_TOO_LONG, "FILENAME_TOO_LONG"}, {ERROR_FILE_IN_USE, "FILE_IN_USE"},
    {ERROR_RECORDSIZE, "RECORDSIZE"}, {ERROR_STARTPOSITION, "STARTPOSITION"},
    {ERROR_TABLE_OVERFLOW, "TABLE_OVERFLOW"}, {ERROR_RECORD_NOT_EXIST, "RECORD_NOT_EXIST"},
    {ERROR_OPERATE_PROHIBIT, "OPERATE_PROHIBIT"}, {ERROR_ALREADY_LOAD, "ALREADY_LOAD"},
    {ERROR_NO_SPACE, "NO_SPACE"}, {ERROR_ITEM_NOT_EXIST, "ITEM_NOT_EXIST"},
    {ERROR_ITEM_ALREADY_EXIST, "ITEM_ALREADY_EXIST"}, {ERROR_ITEM_OVERFLOW, "ITEM_OVERFLOW"},
    {ERROR_SOCKET_NOT_CONNECTED, "SOCKET_NOT_CONNECTED"}, {ERROR_MSGSIZE, "MSGSIZE"},
    {ERROR_BUFFER_SIZE, "BUFFER_SIZE"}, {ERROR_PARAMETER_SIZE, "PARAMETER_SIZE"},
    {STRING_TOO_LONG, "STRING_TOO_LONG"}, {BUFFER_TOO_SMALL, "BUFFER_TOO_SMALL"},
    {ERROR_INVALID_PARAMETER, "INVALID_PARAMETER"}, {ERROR_INVALID_RESPONSE, "INVALID_RESPONSE"},
    {ERROR_BUFFER_TOO_SMALL, "BUFFER_TOO_SMALL2"}, {ERROR_TAG_NOT_EXIST, "TAG_NOT_EXIST"},
    {ERROR_WAIT_TIMEOUT, "WAIT_TIMEOUT"}, {ERROR_RESPONSE_TIMEOUT, "RESPONSE_TIMEOUT"},
    {ERROR_REQUEST_QUEUE_FULL, "REQUEST_QUEUE_FULL"},
};

const char* err_name(unsigned code)
{
    for (const auto& e : kErrNames)
        if (e.code == code) return e.name;
    static thread_local char buf[32];
    snprintf(buf, sizeof(buf), "ERR_%u", code);
    return buf;
}

// ---------------------------------------------------------------------------
// XFAIL 缺陷ID → 登记表标签（与 cases/xfail_ids.h 的枚举、方案 §9 的表一一对应）
// ---------------------------------------------------------------------------
struct BugLabel { int id; const char* label; };
static const BugLabel kBugLabels[] = {
    {1, "C1"}, {2, "C2"}, {3, "C3"}, {4, "C4"}, {5, "C5"}, {6, "C6"},
    {7, "S1"}, {8, "S2"},
    {9, "P1"}, {10, "P2"}, {11, "P3"},
    {12, "G1"}, {13, "M1"}, {14, "B1"},
    {15, "L1"},
};

const char* xfail_label(int bugId)
{
    for (const auto& b : kBugLabels)
        if (b.id == bugId) return b.label;
    return "?";
}

// ---------------------------------------------------------------------------
// TestContext
// ---------------------------------------------------------------------------
void TestContext::fail(const char* file, int line, const char* fmt, ...)
{
    failures++;
    const char* base = strrchr(file, '/');
    base = base ? base + 1 : file;
    fprintf(stdout, "    [FAIL] %s:%d  ", base, line);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fprintf(stdout, "\n");
    fflush(stdout);
}

void TestContext::skip(const std::string& reason)
{
    skipped = true;
    skipReason = reason;
}

ServerFixture& TestContext::server() { return runner->shared_server(); }

// ---------------------------------------------------------------------------
// Runner
// ---------------------------------------------------------------------------
static uint64_t hash64(const char* s)
{
    uint64_t h = 1469598103934665603ull;  // FNV-1a
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ull; }
    return h;
}

static uint64_t now_ns()
{
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

Runner::Runner(const RunOptions& opt) : opt_(opt)
{
    if (!opt_.seedSet) opt_.seed = (uint64_t)time(nullptr) ^ ((uint64_t)getpid() << 32);
    if (opt_.all) { opt_.runDestructive = true; opt_.runStress = true; }
}

Runner::~Runner()
{
    if (shared_) { delete shared_; shared_ = nullptr; }
}

ServerFixture& Runner::shared_server()
{
    if (!shared_) {
        ServerConfig cfg;
        cfg.threads = 2;
        cfg.asan = opt_.expectAsan;
        cfg.scale = opt_.scale;
        cfg.populateFixtures = true;
        cfg.gplatPath = opt_.gplatPath;  // 空=自动探测 /proc/self/exe 同目录
        shared_ = new ServerFixture(cfg);
        if (!shared_->start()) {
            std::string err = shared_->startup_error();
            delete shared_;
            shared_ = nullptr;
            throw std::runtime_error("shared server failed to start: " + err);
        }
    }
    return *shared_;
}

void Runner::ensure_shared_alive()
{
    if (shared_ && !shared_->running()) {
        // 共享 server 意外退出（通常意味着被某用例触发的真实缺陷打崩）。
        // 丢弃并在下次 server() 时重建，避免后续用例全部误失败。
        printf("    [warn] shared server died; recreating for subsequent tests\n");
        if (auto* ctx = current_context())
            ctx->fail(__FILE__, __LINE__, "shared server exited unexpectedly");
        delete shared_;
        shared_ = nullptr;
    }
}

bool Runner::selected(const TestCase& tc, std::string& skipReason) const
{
    if (!opt_.filter.empty() && strstr(tc.name, opt_.filter.c_str()) == nullptr)
        return false;
    if (opt_.excludeMask != 0 && (tc.tags & opt_.excludeMask) != 0)
        return false;
    if (opt_.tagMask != 0 && (tc.tags & opt_.tagMask) == 0)
        return false;
    if (!opt_.list && !opt_.runDestructive && (tc.tags & TAG_DESTRUCTIVE)) { skipReason = "destructive"; return false; }
    if (!opt_.list && !opt_.runStress && (tc.tags & TAG_STRESS)) { skipReason = "stress"; return false; }
    return true;
}

int Runner::run()
{
    const auto& cases = all_cases();

    if (opt_.list) {
        printf("Registered test cases (%zu):\n", cases.size());
        for (const auto& tc : cases) {
            std::string reason;
            if (!selected(tc, reason)) continue;
            char xf[32] = "";
            if (tc.xfailBug) snprintf(xf, sizeof(xf), "  (XFAIL %s)", xfail_label(tc.xfailBug));
            printf("  %-40s [%s]%s\n", tc.name, tags_to_string(tc.tags).c_str(), xf);
        }
        return 0;
    }

    // 选择与门控统计
    std::vector<const TestCase*> run_list;
    int hiddenDestructive = 0, hiddenStress = 0;
    for (const auto& tc : cases) {
        std::string reason;
        if (selected(tc, reason)) run_list.push_back(&tc);
        else if (reason == "destructive") hiddenDestructive++;
        else if (reason == "stress") hiddenStress++;
    }

    if (run_list.empty()) {
        fprintf(stderr, "No test cases selected; check filters and gate switches.\n");
        return 2;
    }
    printf("[seed %llu]  running %zu of %zu cases",
           (unsigned long long)opt_.seed, run_list.size(), cases.size());
    if (hiddenDestructive || hiddenStress)
        printf("  (hidden: %d destructive, %d stress)", hiddenDestructive, hiddenStress);
    printf("\n\n");
    fflush(stdout);

    int nPass = 0, nFail = 0, nXfail = 0, nXpass = 0, nSkip = 0;

    for (const TestCase* tcp : run_list) {
        const TestCase& tc = *tcp;
        TestContext ctx;
        ctx.name = tc.name;
        ctx.seed = opt_.seed ^ hash64(tc.name);
        ctx.scale = opt_.scale;
        ctx.expectAsan = opt_.expectAsan;
        ctx.runner = this;

        printf("[ RUN  ] %s\n", tc.name);
        fflush(stdout);
        uint64_t t0 = now_ns();
        activeContext = &ctx;
        try {
            tc.fn(ctx);
        } catch (const AbortTest&) {
            // 硬断言已记录失败
        } catch (const std::exception& e) {
            ctx.fail(tc.file, tc.line, "uncaught exception: %s", e.what());
        } catch (...) {
            ctx.fail(tc.file, tc.line, "uncaught unknown exception");
        }
        ensure_shared_alive();
        activeContext = nullptr;
        double ms = (now_ns() - t0) / 1e6;

        Outcome oc;
        if (ctx.failures > ctx.expectedFailures) oc = Outcome::FAIL;
        else if (ctx.skipped) oc = Outcome::SKIP;
        else if (tc.xfailBug != 0) oc = (ctx.expectedFailures > 0) ? Outcome::XFAIL : Outcome::XPASS;
        else oc = (ctx.failures > 0) ? Outcome::FAIL : Outcome::PASS;

        const char* tag = "";
        switch (oc) {
            case Outcome::PASS:  tag = "[ PASS ]"; nPass++; break;
            case Outcome::FAIL:  tag = "[ FAIL ]"; nFail++; break;
            case Outcome::XFAIL: tag = "[XFAIL ]"; nXfail++; break;
            case Outcome::XPASS: tag = "[XPASS ]"; nXpass++; break;
            case Outcome::SKIP:  tag = "[ SKIP ]"; nSkip++; break;
        }
        printf("%s %s  (%.0f ms, %d checks", tag, tc.name, ms, ctx.checks);
        if (oc == Outcome::XFAIL || oc == Outcome::XPASS)
            printf(", bug %s(#%d)", xfail_label(tc.xfailBug), tc.xfailBug);
        if (oc == Outcome::SKIP && !ctx.skipReason.empty()) printf(", %s", ctx.skipReason.c_str());
        if (oc == Outcome::XPASS) printf(" — BUG FIXED? update registry");
        printf(")\n\n");
        fflush(stdout);

    }

    if (shared_) {
        TestContext cleanup;
        cleanup.name = "suite.server_teardown";
        activeContext = &cleanup;
        delete shared_;
        shared_ = nullptr;
        activeContext = nullptr;
        if (cleanup.failures) {
            nFail++;
            printf("[ FAIL ] suite.server_teardown\n");
        }
    }

    printf("=== Summary: %d passed, %d failed, %d xfail, %d xpass, %d skipped ===\n",
           nPass, nFail, nXfail, nXpass, nSkip);
    if (hiddenDestructive || hiddenStress)
        printf("    (%d destructive + %d stress hidden; use `make test-destructive` / `make test-stress`)\n",
               hiddenDestructive, hiddenStress);
    if (nXpass > 0)
        printf("    NOTE: %d XPASS — a registered bug now passes; review the XFAIL registry.\n", nXpass);

    // 退出码：有 FAIL 或 XPASS → 非 0
    return (nFail > 0 || nXpass > 0) ? 1 : 0;
}

}  // namespace ts
