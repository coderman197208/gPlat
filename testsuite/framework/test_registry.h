// test_registry.h —— 用例注册表、标签、XFAIL 登记与运行器
//
// 设计要点：
//   * 每个用例是一个带标签位(tags)与 XFAIL 缺陷ID(xfailBug)的函数。
//   * 用 TEST()/TEST_XFAIL() 宏在静态构造期把用例登记进全局注册表（无需手工列清单）。
//   * 运行器按 --filter/--tag/--list/--seed/--scale 等筛选并执行，输出 PASS/FAIL/XFAIL/XPASS/SKIP。
//   * 断言用异常(AbortTest)实现硬中止（见 assertions.h），从而正确析构用例内的 RAII 对象。
//
// 语义约定（关键）：
//   普通 TEST 断言的是“正确行为”。PASS=符合预期；FAIL=发现缺陷。
//   TEST_XFAIL 同样断言“正确行为”，但因命中已登记缺陷，今天必然失败 → 记为 XFAIL（套件保持绿色）。
//   一旦缺陷被修复，断言转为通过 → XPASS，提醒维护者更新《已知缺陷登记表》。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ts {

// ---------------------------------------------------------------------------
// 标签位：一个用例可带多个标签（按位或）。运行器据此做集合筛选与默认门控。
// ---------------------------------------------------------------------------
enum TestTag : uint32_t {
    TAG_BOARD       = 1u << 0,   // BOARD 基础/边界
    TAG_QUEUE       = 1u << 1,   // QUEUE 基础/边界
    TAG_STRING      = 1u << 2,   // String 类型上限
    TAG_PUBSUB      = 1u << 3,   // 订阅/等待/延迟投递
    TAG_GETRESP     = 1u << 4,   // 请求/响应
    TAG_PERSIST     = 1u << 5,   // 持久化/重启
    TAG_MEMORY      = 1u << 6,   // 内存趋势（RSS/fd），非 ASan
    TAG_CHURN       = 1u << 7,   // BOARD 频繁建删稳定性
    TAG_CONN        = 1u << 8,   // 连接 churn
    TAG_STRESS      = 1u << 9,   // 高 QPS / 高并发基准（默认不跑）
    TAG_PROTOCOL    = 1u << 10,  // 裸协议模糊（破坏性）
    TAG_DESTRUCTIVE = 1u << 11,  // 可能打崩 server/client，需隔离，默认不跑
    TAG_ASAN        = 1u << 12,  // 泄漏敏感，需 ASan 构建才有意义
    TAG_WHITEBOX    = 1u << 13,  // 使用 board_inspector 直接 mmap 校验
};

// 把 --tag 的名字（如 "board"）翻译为标签位；未知返回 0。
uint32_t tag_from_name(const std::string& name);
// 把标签位集合翻译成可读字符串（用于 --list）。
std::string tags_to_string(uint32_t tags);
// 把 higplat 错误码翻译成名字（诊断用）；未知返回静态 "ERR_<n>" 串。
const char* err_name(unsigned code);
// 把 XFAIL 缺陷ID（见 cases/xfail_ids.h）翻译成登记表标签（如 "C1"）；未知返回 "?"。
const char* xfail_label(int bugId);

// ---------------------------------------------------------------------------
// 前置声明：TestContext 依赖的服务（server_fixture 在 .cpp 中按需包含）
// ---------------------------------------------------------------------------
class ServerFixture;
struct TestContext;

using TestFn = void (*)(TestContext&);

struct TestCase {
    const char* name;      // 唯一名字（建议 "模块.子项" 风格）
    const char* file;      // 源文件（__FILE__）
    int         line;      // 行号（__LINE__）
    uint32_t    tags;      // 标签位
    int         xfailBug;  // 0=预期通过；>0=命中登记表缺陷ID，预期失败
    TestFn      fn;        // 用例函数
};

// 注册一个用例（由 Registrar 在静态构造期调用）。
void register_case(const TestCase& tc);
// 取得全部已注册用例（运行器用）。
const std::vector<TestCase>& all_cases();

// 静态构造期登记器。
struct Registrar {
    explicit Registrar(const TestCase& tc) { register_case(tc); }
};

#define TS_CONCAT_INNER(a, b) a##b
#define TS_CONCAT(a, b) TS_CONCAT_INNER(a, b)

// TEST(name, tags) { ... 用例体，形参名固定为 ctx ... }
#define TEST(NAME, TAGS)                                                        \
    static void TS_CONCAT(ts_fn_, __LINE__)(ts::TestContext&);                  \
    static ::ts::Registrar TS_CONCAT(ts_reg_, __LINE__){                        \
        ::ts::TestCase{NAME, __FILE__, __LINE__, (TAGS), 0,                     \
                       &TS_CONCAT(ts_fn_, __LINE__)}};                          \
    static void TS_CONCAT(ts_fn_, __LINE__)(ts::TestContext& ctx)

// TEST_XFAIL(name, tags, bugid) —— 命中已知缺陷、预期失败的用例。
#define TEST_XFAIL(NAME, TAGS, BUGID)                                           \
    static void TS_CONCAT(ts_fn_, __LINE__)(ts::TestContext&);                  \
    static ::ts::Registrar TS_CONCAT(ts_reg_, __LINE__){                        \
        ::ts::TestCase{NAME, __FILE__, __LINE__, (TAGS), (BUGID),               \
                       &TS_CONCAT(ts_fn_, __LINE__)}};                          \
    static void TS_CONCAT(ts_fn_, __LINE__)(ts::TestContext& ctx)

// ---------------------------------------------------------------------------
// 运行选项与运行器
// ---------------------------------------------------------------------------
struct RunOptions {
    std::string filter;                 // 名字子串过滤（空=不过滤）
    uint32_t    tagMask = 0;            // --tag 的并集（0=不限）
    uint32_t    excludeMask = 0;        // --exclude-tag 的并集（命中即排除，优先级高于 tagMask）
    uint64_t    seed = 0;              // 随机种子（0=用时间）
    bool        seedSet = false;
    int         scale = 1;             // 规模系数（压力/churn 用例按需放大）
    bool        list = false;          // 只列不跑
    bool        runDestructive = false; // 允许破坏性用例
    bool        runStress = false;      // 允许压力/基准用例
    bool        expectAsan = false;     // 期望运行在 ASan 构建下（启用泄漏判定）
    bool        all = false;            // 不做默认门控，全部运行
    std::string gplatPath;             // gplat 可执行路径（空=自动探测）
};

enum class Outcome { PASS, FAIL, XFAIL, XPASS, SKIP };

// 运行器：持有一个按需启动的“共享 server”，供大多数功能用例复用；
// 需要特殊配置/会崩溃的用例自建 ServerFixture，不走共享实例。
class Runner {
public:
    explicit Runner(const RunOptions& opt);
    ~Runner();

    int run();  // 返回进程退出码：有 FAIL 或 XPASS → 非 0

    // 供 TestContext::server() 惰性取用的共享实例（首次使用时启动）。
    ServerFixture& shared_server();

    const RunOptions& options() const { return opt_; }

private:
    bool selected(const TestCase& tc, std::string& skipReason) const;
    void ensure_shared_alive();

    RunOptions opt_;
    ServerFixture* shared_ = nullptr;  // 惰性；Runner 析构时停止
};

// ---------------------------------------------------------------------------
// 用例上下文：失败计数、随机种子、规模、以及对共享 server 的访问入口。
// ---------------------------------------------------------------------------
struct TestContext {
    const char* name = "";
    uint64_t    seed = 0;
    int         scale = 1;
    bool        expectAsan = false;

    int  failures = 0;   // 软失败计数（CHECK/EXPECT/ASSERT 记录）
    int  expectedFailures = 0;  // 仅 BUG_CHECK 精确命中的登记缺陷
    int  checks = 0;     // 断言总数
    bool skipped = false;
    std::string skipReason;

    Runner* runner = nullptr;

    // 记录一次失败（printf 风格）；立即打印带文件:行的信息。
    void fail(const char* file, int line, const char* fmt, ...)
        __attribute__((format(printf, 4, 5)));
    // 主动跳过本用例（例如需要 ASan 却未在 ASan 构建下运行）。
    void skip(const std::string& reason);

    // 取共享 server（惰性启动）。需特殊配置的用例请自建 ServerFixture。
    ServerFixture& server();
};

// 由 ASSERT 抛出，用于硬中止当前用例（能正确析构 RAII 对象）。
struct AbortTest {};

// 仅运行器主线程设置，供 fixture 析构时把停机/ASan 错误计入当前用例。
TestContext* current_context();

}  // namespace ts
