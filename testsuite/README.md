# gPlat 自动化测试套件（testsuite）

针对 gPlat 核心引擎（`gplat` 服务端 + `libhigplat` 客户端）的端到端回归 / 压力 / 破坏性测试框架。
自研、零第三方依赖，用例按**正常 / 边界 / 非法 / 极端**四象限覆盖任务要求的全部 API。

> 对应任务：[`Doc/tasks/20261002-02.md`](../Doc/tasks/20261002-02.md)，方案：[`Doc/tasks/20261002-02-plan.md`](../Doc/tasks/20261002-02-plan.md)

---

## 隔离纪律（务必了解）

套件**绝不触碰**生产实例或仓库数据：

- 每个 server 实例自建 `mkdtemp` 临时 sandbox（`config/ qbdfile/ logs/ bin/`），teardown 自动清理。
- 监听端口从 **18777** 起自动探测空闲端口——**不碰 8777 上你正在运行的 gplat，也不碰仓库的 `qbdfile/`**。
- fixtures 由本地 API 在 forked 子进程里写入 sandbox，不污染 harness 进程的全局态。

即使本机有 gplat 在跑，直接 `make test` 也安全。

---

## 构建

```bash
make testsuite          # 编译套件（常规构建）→ bin/testsuite、bin/gen_fixtures
make ASAN=1 asan-build  # 编 ASan 变体 → build/asan/{bin,lib}（rpath 同级，自解析 ../lib）
```

常规构建输出在 `bin/`、`lib/`；ASan 构建隔离到 `build/asan/`，互不影响。
套件静态链接 `libhigplat.so`（用其网络 API）；白盒校验器额外直接 `mmap` sandbox 文件验结构。

---

## 运行

| 命令 | 作用 | 判据 |
|---|---|---|
| `make test` | 常规回归（隐藏破坏性/压力） | 全绿即通过；XFAIL 不计失败 |
| `make test-asan` | ASan 重编 + 跑泄漏敏感用例 | LSan 零残留（减基线后） |
| `make test-destructive` | 破坏性用例（fork 隔离，逐个重启 server） | 命中登记缺陷 → XFAIL |
| `make test-stress` | 高 QPS / 高并发 / churn 基准 | 正确性 + 无 fd 泄漏；性能仅报告 |

直接运行 `bin/testsuite` 可用更细的筛选：

```
-f, --filter <子串>      只跑名字含该子串的用例
-t, --tag <名>           只跑含该标签的用例（可重复，并集）
-x, --exclude-tag <名>   排除标签（优先于 --tag）
-l, --list               只列出匹配用例，不执行
    --run-destructive    放行破坏性用例（默认隐藏）
    --run-stress         放行压力用例（默认隐藏）
    --all                等价于同时放行上面两项
    --seed <u64>         固定随机种子（复现失败）
    --scale <n>          规模系数（压力/churn 放大，默认 1）
    --expect-asan        期望运行于 ASan 构建：启用泄漏判定
    --gplat <path>       指定 gplat 可执行路径（默认取本程序同目录）
```

标签：`board queue string pubsub getresp persist memory churn conn stress protocol destructive asan whitebox`

**复现**：每次运行打印 `[seed N]`；带 `--seed N` 即可精确重放随机负载。

---

## 用例结果语义

| 结果 | 含义 |
|---|---|
| `PASS` | 断言的“正确行为”成立 |
| `FAIL` | 发现**非预期**缺陷（退出码非 0） |
| `XFAIL` | 命中《已知缺陷登记表》，今日必失败——套件保持绿色 |
| `XPASS` | 登记为缺陷却通过了 → **缺陷可能已修复，请更新登记表**（退出码非 0） |
| `SKIP` | 前置条件不满足（如需 ASan 构建却未启用） |

断言宏（`framework/assertions.h`）：`CHECK`（软）/ `ASSERT`（硬，中止用例）/ `EXPECT_OK` / `EXPECT_ERR`（错误码专用）/ `REQUIRE_OK`（前置条件）/ **`BUG_CHECK`**（XFAIL 专用，只豁免这一个检查；其它断言失败仍是 FAIL）。

> 规模：当前共 **78** 个用例。常规 `make test` 全绿，仅 `G1` 记 XFAIL、ASan 专用 `mem.*` 在非 ASan 下 SKIP。

---

## 已知缺陷登记表（XFAIL）

用例对这些**真实缺陷**只报告、不改源码。缺陷被修复后对应用例翻转为 `XPASS` 提醒更新本表。
符号常量见 [`cases/xfail_ids.h`](cases/xfail_ids.h)。

| ID | 位置 | 现象 | 覆盖用例 |
|---|---|---|---|
| B1 | `DeleteItem` memmove | 不持条带锁，与 readb 竞态 → 撕裂读 | `board_churn.concurrent_delete_race` |
| G1 | `m_mapResponseOwner` | 永不 erase → response_tag 无法复用、无界累积 | `getresp.response_owner_never_erased` |

**特征化报告（非 XFAIL，RSS 趋势不适合作门控）**：

| ID | 现象 | 覆盖用例 |
|---|---|---|
| M1 | 连接池 free-list 抽空即 new、按峰值撑大不缩 | `mem.connpool_hwm_trend` |
| P3 | 只订不取 → `m_listPost` 堆积（已由 `Sock_MaxPendingPost` 封顶，默认 1000，满后丢弃最新事件；`pubsub.pending_queue_capped` 门控上限，本用例仅观察 RSS） | `pubsub.listpost_unbounded_trend` |
| L1 | 致命启动失败的退出码（当前实测已为 1） | `persist.corrupt_exit_code` |

---

## 泄漏判定策略（避免误报）

gplat 在**关闭时、带在连接客户端**存在“逻辑 shutdown-leak”（`m_listPost`/收发缓冲），LSan 会如实报告。套件据方案 §5 区分：

1. **基线法**：`mem.baseline_clean` 记录空闲 + 干净断开 + 正常 SIGTERM 的 LSan 指纹。
2. **分离 shutdown-leak**：`mem.live_conn_shutdown_leak` 显式复现“带连接被 SIGTERM”的已知泄漏（`allowLeaks` 放行并特征化），作为“修复后应消失”的探针。
3. **趋势法**：`proc_sampler` 采样 worker `VmRSS`/fd 斜率，抓 LSan 看不见的**可达**无界增长（连接池 M1、`m_listPost` P3）；RSS 回缩依赖 glibc trim，故只报告。

`mem.crud_no_residual` 是权威闸门：全量 CRUD 后 ASan/LSan **零残留**才算通过。

---

## Fixtures（tag / queue 创建“脚本”）

所有预建 tag/queue 的名字与尺寸集中在 **单一事实源** [`framework/fixtures.h`](framework/fixtures.h)（`ts::fx::`），
由 [`framework/fixtures.cpp`](framework/fixtures.cpp) 的 `create_all_fixtures()` 用**本地 API**（直接 mmap QBD）建好，
涵盖 CLI 不支持的 **ASCII 队列**与特定 `itemsize` 的字符串 tag。`ServerFixture::start()` 启动前自动在 sandbox 里建好，开箱即用。

独立生成器 `bin/gen_fixtures`（源 [`scripts/gen_fixtures.cpp`](scripts/gen_fixtures.cpp)）可按需在任意目录单独生成同一套 fixtures：

```bash
bin/gen_fixtures <qbdfile目录> [scale]
```

夹具目录（节选，详见 `fixtures.h`）：标量/数组/二进制 tag（`T_I32`…`T_BINMAX`）、字符串 tag（`S_CAP1`…`S_CAPMAX`）、
请求/响应 tag（`REQ_A`/`RSP_A`）、四类队列（NORMAL/SHIFT × BINARY/ASCII）+ 大记录队列 `Q_BIG`。

---

## 框架模块（`framework/`）

| 文件 | 职责 |
|---|---|
| `test_registry.*` | 用例注册表、标签、XFAIL 登记、运行器（筛选/结果分类/退出码） |
| `assertions.h` | `CHECK`/`ASSERT`/`EXPECT_OK`/`EXPECT_ERR`/`REQUIRE_OK`/`BUG_CHECK` |
| `fork_runner.*` | `run_in_fork`：预期崩溃/挂死的用例在子进程隔离执行，捕获信号/退出码/ASan 签名 |
| `server_fixture.*` | sandbox 搭建 + gplat 启停 + 就绪探针 + ASan 报告解析 + `ScopedConn` |
| `proc_sampler.*` | 读 `/proc/<pid>/{status,fd}` 做 RSS/fd 趋势（最小二乘斜率） |
| `board_inspector.*` | 白盒：直接 mmap sandbox 的 BOARD 文件校验结构不变量 |
| `workload.*` | 带种子 `Rng`、CRC32 自校验 payload、延迟直方图（p50/p99/p999） |
| `net_raw.*` | 裸 socket + 手工 MSGHEAD 收发（协议模糊 / 就绪探针） |
| `fixtures.*` | 夹具目录单一事实源 + 本地 API 建夹具 |
| `ts.h` | 伞头：用例只需 `#include "framework/ts.h"` |

---

## 加一个新用例

```cpp
#include "framework/ts.h"
using namespace ts;

TEST("mymodule.my_case", TAG_BOARD)          // 预期通过
{
    ScopedConn c(ctx.server());              // 共享 server（功能用例）
    ASSERT(ctx, c.ok(), "connect failed");
    unsigned err = 0;
    int32_t v = 42;
    REQUIRE_OK(ctx, writeb(c.fd(), fx::TAG_I32, &v, sizeof(v), &err), err);
    // ...
}
```

- 命中已知缺陷用 `TEST_XFAIL("name", tags, BUG_XX)` + `BUG_CHECK(...)`（见 `cases/xfail_ids.h`）。
- 需特殊配置 / 会崩溃的用例自建 `ServerFixture`（见 `test_memory`/`test_conn_churn`/`test_protocol`），
  破坏性的额外打 `TAG_DESTRUCTIVE` 并在 fork 子进程里跑。
- 放到 `cases/` 下任一 `.cpp` 即被自动发现（`TEST`/`TEST_XFAIL` 宏在静态构造期自注册，无需手工列清单）。

---

## 目录

```
testsuite/
├── main.cpp            # CLI 解析 → Runner
├── framework/          # 框架设施（见上表）
├── cases/              # 用例（test_board/queue/string/pubsub/getresponse/
│                       #       persistence/memory/board_churn/conn_churn/stress/protocol/framework）
│   └── xfail_ids.h     # XFAIL 缺陷ID 符号常量
└── scripts/
    └── gen_fixtures.cpp # 独立 fixtures 生成器
```
