// assertions.h —— 断言宏（软/硬 + 错误码专用）
//
// 约定：所有宏的第一个参数都是 TestContext&（名为 ctx）。
//   * CHECK   软断言：条件为假时记一次失败并继续（适合一个用例里多点校验）。
//   * ASSERT  硬断言：条件为假时记失败并抛 AbortTest 结束本用例（防止后续解引用崩溃）。
//   * EXPECT_OK   期望 API 调用返回 true 且 error==0（成功路径）。软。
//   * EXPECT_ERR  期望 API 调用返回 false 且 error==指定码（边界/非法路径的主力）。软。
//   * REQUIRE_OK  同 EXPECT_OK，但失败即 AbortTest（前置条件，如连接必须成功）。
//
// 设计动机：
//   gPlat 的 C API 统一是 `bool f(..., unsigned int* error)`，失败返回 false 并置 error。
//   EXPECT_OK/EXPECT_ERR 把“返回值 + error 码”两件事一次校验，并自动打印调用文本与错误名，
//   让边界用例（第 7 节四象限里的 [B]/[I]）写起来简洁且诊断信息完整。
#pragma once

#include "test_registry.h"

// 记录一次无条件失败（带位置与 printf 风格信息）。
#define TS_FAILF(ctx, ...) (ctx).fail(__FILE__, __LINE__, __VA_ARGS__)

// 软断言：失败记录并继续。
#define CHECK(ctx, cond, ...)                                                   \
    do {                                                                        \
        (ctx).checks++;                                                         \
        if (!(cond)) (ctx).fail(__FILE__, __LINE__, __VA_ARGS__);               \
    } while (0)

// XFAIL 只豁免这个检查；启动失败、超时、其他断言失败仍然是 FAIL。
#define BUG_CHECK(ctx, cond, ...)                                               \
    do {                                                                        \
        (ctx).checks++;                                                         \
        if (!(cond)) {                                                          \
            (ctx).expectedFailures++;                                           \
            (ctx).fail(__FILE__, __LINE__, __VA_ARGS__);                          \
        }                                                                       \
    } while (0)

// 硬断言：失败记录并抛 AbortTest 终止本用例。
#define ASSERT(ctx, cond, ...)                                                  \
    do {                                                                        \
        (ctx).checks++;                                                         \
        if (!(cond)) {                                                          \
            (ctx).fail(__FILE__, __LINE__, __VA_ARGS__);                        \
            throw ::ts::AbortTest{};                                            \
        }                                                                       \
    } while (0)

// 期望成功：call 返回 true 且 err==0。err 必须是调用方的可写左值。
#define EXPECT_OK(ctx, call, err)                                               \
    do {                                                                        \
        (ctx).checks++;                                                         \
        (err) = 0;                                                              \
        bool _ts_ok = (call);                                                   \
        if (!_ts_ok || (err) != 0)                                             \
            (ctx).fail(__FILE__, __LINE__,                                      \
                       "EXPECT_OK(%s): ret=%d error=%u(%s)", #call,            \
                       (int)_ts_ok, (unsigned)(err),                           \
                       ::ts::err_name((unsigned)(err)));                        \
    } while (0)

// 期望失败并命中特定错误码：call 返回 false 且 err==code。
#define EXPECT_ERR(ctx, call, err, code)                                        \
    do {                                                                        \
        (ctx).checks++;                                                         \
        (err) = 0;                                                              \
        bool _ts_ok = (call);                                                   \
        if (_ts_ok || (err) != (unsigned)(code))                               \
            (ctx).fail(__FILE__, __LINE__,                                      \
                       "EXPECT_ERR(%s): want error=%u(%s), got ret=%d error=%u(%s)", \
                       #call, (unsigned)(code), ::ts::err_name((unsigned)(code)), \
                       (int)_ts_ok, (unsigned)(err),                           \
                       ::ts::err_name((unsigned)(err)));                        \
    } while (0)

// 前置条件版 EXPECT_OK：失败即终止用例（常用于 connect/建 fixture）。
#define REQUIRE_OK(ctx, call, err)                                              \
    do {                                                                        \
        (ctx).checks++;                                                         \
        (err) = 0;                                                              \
        bool _ts_ok = (call);                                                   \
        if (!_ts_ok || (err) != 0) {                                           \
            (ctx).fail(__FILE__, __LINE__,                                      \
                       "REQUIRE_OK(%s): ret=%d error=%u(%s)", #call,           \
                       (int)_ts_ok, (unsigned)(err),                           \
                       ::ts::err_name((unsigned)(err)));                        \
            throw ::ts::AbortTest{};                                            \
        }                                                                       \
    } while (0)
