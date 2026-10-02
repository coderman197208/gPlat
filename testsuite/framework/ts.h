// ts.h —— 测试套件伞头：用例只需 #include "framework/ts.h"
//
// 汇总框架全部公共设施 + 被测网络 API（higplat.h）。
// 各用例文件典型写法：
//     #include "framework/ts.h"
//     using namespace ts;
//     TEST("board.scalar_roundtrip", TAG_BOARD) { ... 用 ctx ... }
#pragma once

// 被测 API（C 网络客户端；本地 mmap API 也在此头内）
#include "higplat.h"
#include "type_code.h"

// 框架设施（同目录，按裸名包含）
#include "test_registry.h"   // TEST/TEST_XFAIL、标签、Runner、TestContext
#include "assertions.h"      // CHECK/ASSERT/EXPECT_OK/EXPECT_ERR/REQUIRE_OK
#include "server_fixture.h"  // ServerFixture / ServerConfig / AsanReport
#include "fixtures.h"        // ts::fx:: 夹具目录 + create_all_fixtures
#include "net_raw.h"         // 裸协议帧（MSGHEAD/send_msg/recv_msg）
#include "workload.h"        // Rng / crc32 / payload / LatencyHistogram / now_nanos
#include "proc_sampler.h"    // ProcSampler（RSS/fd 趋势）
#include "fork_runner.h"     // run_in_fork（破坏性/隔离用例）
