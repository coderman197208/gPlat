// test_board_churn.cpp —— BOARD 频繁建删 tag 后的结构稳定性（方案 §7.10 + §6）
//
// 核心手段：每轮随机建删/改写后，用白盒校验器 board_inspector 直接只读 mmap sandbox 的
//   BOARD 文件复算结构不变量（索引计数、数据区/类型区紧致、探测可达、无重名、墓碑统计），
//   同时维护一张“影子模型”（name→{数据字节, 类型字节}）逐 tag 比对——既校验结构自洽，
//   又证明 DeleteItem 的 memmove 搬移没有损坏邻接 tag 的数据/类型。
//
// 为什么用私有 server：这些用例要把 BOARD 建删到特定形态并直接 mmap 其文件，必须与共享实例
//   隔离（独立 sandbox + 自动空闲端口，绝不碰 8777 生产实例）。沿用 persistence/memory 的
//   PRIVATE_SERVER 模式。server 仍预建标准 fixtures（故 BOARD 初始含 ~20 个 fx tag）——
//   影子模型只跟踪我们自建的 "CH_" 前缀 tag；不变量是全局的，对 fixtures + churn tag 一并成立。
//
// 覆盖：
//   [N] random_rounds          —— 带种子随机 N 轮 {建 k/删 j/改写 w}，每轮全量白盒+影子比对
//   [B] equal_charsum_cluster  —— 等字符和名字簇（相同探测序列）建删，压探测链/墓碑跳过
//   [B] delete_to_empty_refill —— 删空（回到 fixture 基线）再建满，验数据/类型区回缩
//   [B] tombstone_reuse        —— 交替建删同名 tag，验墓碑槽被复用（不泄漏槽）
//   [B1 已修复] concurrent_delete_race —— 多连接并发建删 + 读，DeleteItem 的 8MB memmove 须与
//                                        readb 互斥，否则撕裂读（破坏性，默认不跑）
//
// 关于 B1 的检测方式（已核对源码）：DeleteItem 仅持全局板锁、ReadB/WriteB 持条带锁，二者不互斥；
//   且 DeleteItem 每次 memmove 长度 = totalsize-sizeof(BOARD_HEAD)-pos-itemsize ≈ 整个 8MB 数据区，
//   race 窗口极大。结构突变（建/删）彼此持全局锁互斥 → 索引结构始终自洽；**只有数据面**会被撕裂。
//   故 B1 用“读回自校验 payload（check_payload 带内嵌 CRC，seq 无关）”捕获撕裂读，而非白盒结构校验。
//   撕裂读不致客户端崩溃、通常也不致服务端崩溃（地址恒在 mmap 内）→ 在**本进程多线程**跑即可，
//   无需 fork（fork_runner 仅用于“打崩客户端”的用例）。
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "framework/ts.h"
#include "framework/board_inspector.h"
#include "cases/xfail_ids.h"

using namespace ts;

namespace {

// 影子模型中单条 tag 的权威内容（建/改写时写入，读回比对）。
struct ShadowTag {
    std::vector<char> data;   // 数据字节（self-checking payload）
    std::vector<char> type;   // 类型描述符字节
};

// 影子模型：name→内容 + 名字向量（供 O(1) 随机抽取 + swap-remove）。
struct ChurnModel {
    std::map<std::string, ShadowTag> tags;
    std::vector<std::string>         names;

    size_t size() const { return names.size(); }
    bool   empty() const { return names.empty(); }

    void add(const std::string& n, ShadowTag st)
    {
        tags[n] = std::move(st);
        names.push_back(n);
    }
    void remove(const std::string& n)
    {
        tags.erase(n);
        auto it = std::find(names.begin(), names.end(), n);
        if (it != names.end()) { *it = names.back(); names.pop_back(); }
    }
    const std::string& random_name(Rng& rng) const
    {
        return names[(size_t)rng.range(0, (int)names.size() - 1)];
    }
};

// 在栈上构造并启动私有 fixture（threads 可指定：并发 race 用例需 ≥2）。
#define PRIVATE_SERVER_T(SRV, NTHREADS)                                          \
    ServerConfig SRV##_cfg;                                                      \
    SRV##_cfg.threads = (NTHREADS);                                             \
    SRV##_cfg.asan = ctx.expectAsan;                                           \
    ServerFixture SRV(SRV##_cfg);                                              \
    ASSERT(ctx, SRV.start(), "private fixture start failed: %s", SRV.startup_error().c_str())

#define PRIVATE_SERVER(SRV) PRIVATE_SERVER_T(SRV, 2)

// BOARD 文件绝对路径（<sandbox>/qbdfile/BOARD）。
std::string board_file(ServerFixture& srv) { return srv.qbd_path() + "/" + fx::BOARD; }

// —— 建一个 churn tag：createtag(带随机类型 blob) + writeb(self-checking payload)，并登记影子 ——
// 注意：网络 createtag 的 typesize 必须 ≥1（typesize==0 会被客户端以 ERROR_PARAMETER_SIZE 拒绝），故恒带类型。
bool churn_create(TestContext& ctx, int fd, ChurnModel& m, const std::string& name,
                  int dataSize, int typeSize, Rng& rng)
{
    unsigned err = 0;
    ShadowTag st;
    st.type.resize(typeSize);
    rng.fill(st.type.data(), (size_t)typeSize);  // 任意类型字节（服务端原样存储，不解析）

    if (!createtag(fd, name.c_str(), dataSize, st.type.data(), typeSize, &err)) {
        TS_FAILF(ctx, "createtag('%s', size=%d, type=%d) failed: err=%u(%s)",
                 name.c_str(), dataSize, typeSize, err, err_name(err));
        return false;
    }
    make_payload(st.data, dataSize, rng.next_u32() & 0x7fffffffu, rng);  // >=16B 自校验负载
    if (!writeb(fd, name.c_str(), st.data.data(), dataSize, &err)) {
        TS_FAILF(ctx, "writeb('%s', %d) after create failed: err=%u(%s)",
                 name.c_str(), dataSize, err, err_name(err));
        return false;
    }
    m.add(name, std::move(st));
    return true;
}

// —— 删一个 churn tag：deletetag + 从影子移除 ——
bool churn_delete(TestContext& ctx, int fd, ChurnModel& m, const std::string& name)
{
    unsigned err = 0;
    if (!deletetag(fd, name.c_str(), &err)) {
        TS_FAILF(ctx, "deletetag('%s') failed: err=%u(%s)", name.c_str(), err, err_name(err));
        return false;
    }
    m.remove(name);
    return true;
}

// —— 改写一个已存在 churn tag：写同尺寸新 payload，更新影子 ——
void churn_rewrite(TestContext& ctx, int fd, ChurnModel& m, const std::string& name, Rng& rng)
{
    auto it = m.tags.find(name);
    if (it == m.tags.end()) return;
    unsigned err = 0;
    int dataSize = (int)it->second.data.size();
    std::vector<char> np;
    make_payload(np, dataSize, rng.next_u32() & 0x7fffffffu, rng);
    if (!writeb(fd, name.c_str(), np.data(), dataSize, &err)) {
        TS_FAILF(ctx, "writeb('%s', %d) rewrite failed: err=%u(%s)",
                 name.c_str(), dataSize, err, err_name(err));
        return;
    }
    it->second.data = std::move(np);
}

// —— 全量白盒 + 影子比对。where 用于定位失败轮次。返回是否全过 ——
bool verify_board(TestContext& ctx, int fd, const std::string& boardFile,
                  const ChurnModel& m, int baseLive, const char* where)
{
    bool allOk = true;

    BoardInspector insp(boardFile);
    if (!insp.ok()) {
        TS_FAILF(ctx, "%s: inspector open failed: %s", where, insp.error().c_str());
        return false;
    }

    // (结构) 全量不变量
    BoardInvariantReport r = insp.validate();
    if (!r.ok) {
        for (const auto& v : r.violations)
            TS_FAILF(ctx, "%s: structural invariant violated: %s", where, v.c_str());
        allOk = false;
    }

    // (计数) 存活数 == fixture 基线 + 影子 churn tag 数（抓幽灵/漏删）
    if (r.liveCount != baseLive + (int)m.size()) {
        TS_FAILF(ctx, "%s: liveCount=%d != baseLive(%d)+churn(%zu)=%d",
                 where, r.liveCount, baseLive, m.size(), baseLive + (int)m.size());
        allOk = false;
    }

    // (数据/类型) 逐 churn tag 三方比对：影子 ↔ 白盒直读 mmap ↔ 网络 readb
    for (const auto& kv : m.tags) {
        const std::string& name = kv.first;
        const ShadowTag&   st   = kv.second;

        if (insp.probe_find(name) < 0) {
            TS_FAILF(ctx, "%s: '%s' not probe-reachable (墓碑截断探测链?)", where, name.c_str());
            allOk = false;
        }
        // 白盒直读数据
        std::vector<char> ib;
        if (!insp.read_item_bytes(name, ib)) {
            TS_FAILF(ctx, "%s: inspector read_item_bytes('%s') failed", where, name.c_str());
            allOk = false;
        } else if (ib.size() != st.data.size() ||
                   memcmp(ib.data(), st.data.data(), st.data.size()) != 0) {
            TS_FAILF(ctx, "%s: '%s' inspector data mismatch (sz %zu/%zu) —— 疑似 memmove 损坏邻接数据",
                     where, name.c_str(), ib.size(), st.data.size());
            allOk = false;
        }
        // 白盒直读类型
        std::vector<char> it;
        if (!insp.read_type_bytes(name, it)) {
            TS_FAILF(ctx, "%s: inspector read_type_bytes('%s') failed", where, name.c_str());
            allOk = false;
        } else if (it != st.type) {
            TS_FAILF(ctx, "%s: '%s' inspector type mismatch (sz %zu/%zu) —— 疑似类型区 memmove 损坏",
                     where, name.c_str(), it.size(), st.type.size());
            allOk = false;
        }
        // 网络 readb（校验服务端读路径同样看到正确字节）
        std::vector<char> nb(st.data.size(), 0);
        unsigned err = 0;
        if (!readb(fd, name.c_str(), nb.data(), (int)nb.size(), &err, nullptr)) {
            TS_FAILF(ctx, "%s: net readb('%s') failed: err=%u(%s)", where, name.c_str(), err, err_name(err));
            allOk = false;
        } else {
            if (memcmp(nb.data(), st.data.data(), st.data.size()) != 0) {
                TS_FAILF(ctx, "%s: '%s' net readb mismatch vs shadow", where, name.c_str());
                allOk = false;
            }
            std::string pe;
            if (!check_payload(nb.data(), (int)nb.size(), -1, &pe)) {
                TS_FAILF(ctx, "%s: '%s' payload integrity broken: %s", where, name.c_str(), pe.c_str());
                allOk = false;
            }
        }
    }
    return allOk;
}

// 读取基线（建任何 churn tag 之前）：fixture 存活数 / 数据区 bump / 类型区 bump / 空槽数。
struct Baseline { int live = 0; int nextpos = 0; int nexttypepos = 0; int empty = 0; int tomb = 0; };

bool read_baseline(TestContext& ctx, const std::string& boardFile, Baseline& b)
{
    BoardInspector insp(boardFile);
    if (!insp.ok()) { TS_FAILF(ctx, "baseline inspector open failed: %s", insp.error().c_str()); return false; }
    BoardInvariantReport r = insp.validate();
    if (!r.ok) {
        for (const auto& v : r.violations) TS_FAILF(ctx, "baseline invariant violated: %s", v.c_str());
        return false;
    }
    b.live = r.liveCount; b.nextpos = r.nextpos; b.nexttypepos = r.nexttypepos;
    b.empty = r.emptySlots; b.tomb = r.tombstones;
    return true;
}

}  // namespace

// ===========================================================================
// [N] 随机 N 轮建删改写 —— 每轮全量白盒 + 影子比对
// ===========================================================================
TEST("board_churn.random_rounds", TAG_CHURN | TAG_WHITEBOX)
{
    PRIVATE_SERVER(srv);
    const std::string bf = board_file(srv);

    Baseline base;
    if (!read_baseline(ctx, bf, base)) return;
    printf("    [info] baseline: live=%d nextpos=%d nexttypepos=%d empty=%d\n",
           base.live, base.nextpos, base.nexttypepos, base.empty);

    ScopedConn c(srv);
    ASSERT(ctx, c.ok(), "connect failed");
    int fd = c.fd();

    Rng rng(ctx.seed);
    ChurnModel m;
    const int ROUNDS  = 40 * ctx.scale;
    const int MAXLIVE = 150;   // 限制同时存活的 churn tag 数（远小于 INDEXSIZE）
    int uid = 0;

    for (int round = 0; round < ROUNDS; round++) {
        // —— 建 k 个（名字唯一，random 尺寸/类型长度）——
        int k = rng.range(1, 12);
        for (int i = 0; i < k && (int)m.size() < MAXLIVE; i++) {
            char nm[GPLAT_TAGNAME_SIZE];
            snprintf(nm, sizeof(nm), "CH_%06d", uid++);
            int ds = rng.range(16, 1024);   // >=16 以容纳 PayloadTag
            int ts = rng.range(1, 40);      // 类型 blob 长度（需 >=1）
            if (!churn_create(ctx, fd, m, nm, ds, ts, rng)) { round = ROUNDS; break; }
        }
        // —— 删 j 个（随机子集，至多半数）——
        int j = rng.range(0, (int)m.size() / 2);
        for (int i = 0; i < j && !m.empty(); i++) {
            std::string victim = m.random_name(rng);  // 按值拷贝：随后 remove 会失效引用
            if (!churn_delete(ctx, fd, m, victim)) { round = ROUNDS; break; }
        }
        // —— 改写 w 个（随机，考验写路径 + 数据不串位）——
        int w = rng.range(0, (int)m.size());
        for (int i = 0; i < w && !m.empty(); i++)
            churn_rewrite(ctx, fd, m, m.random_name(rng), rng);

        // —— 每轮全量白盒 + 影子比对 ——
        char where[48];
        snprintf(where, sizeof(where), "round %d (live=%zu)", round, m.size());
        if (!verify_board(ctx, fd, bf, m, base.live, where)) {
            TS_FAILF(ctx, "stop at %s (seed=%llu 可精确重放)", where, (unsigned long long)ctx.seed);
            break;
        }
    }

    // —— 收尾：删空所有 churn tag，验回到 fixture 基线（数据/类型区完全回缩）——
    std::vector<std::string> remaining = m.names;
    for (const auto& n : remaining) churn_delete(ctx, fd, m, n);
    CHECK(ctx, m.empty(), "model should be empty after deleting all");

    BoardInspector insp(bf);
    ASSERT(ctx, insp.ok(), "final inspector open failed: %s", insp.error().c_str());
    BoardInvariantReport r = insp.validate();
    for (const auto& v : r.violations) TS_FAILF(ctx, "final: %s", v.c_str());
    CHECK(ctx, r.liveCount == base.live, "final liveCount=%d != baseline %d", r.liveCount, base.live);
    CHECK(ctx, r.nextpos == base.nextpos, "final nextpos=%d != baseline %d (数据区未回缩)",
          r.nextpos, base.nextpos);
    CHECK(ctx, r.nexttypepos == base.nexttypepos, "final nexttypepos=%d != baseline %d (类型区未回缩)",
          r.nexttypepos, base.nexttypepos);
    printf("    [info] after delete-all: live=%d nextpos=%d nexttypepos=%d tomb=%d empty=%d\n",
           r.liveCount, r.nextpos, r.nexttypepos, r.tombstones, r.emptySlots);
}

// ===========================================================================
// [B] 等字符和名字簇 —— 相同 hash1/hash2 → 同一探测序列，压探测链 + 墓碑跳过
// ===========================================================================
TEST("board_churn.equal_charsum_cluster", TAG_CHURN | TAG_WHITEBOX)
{
    PRIVATE_SERVER(srv);
    const std::string bf = board_file(srv);
    Baseline base;
    if (!read_baseline(ctx, bf, base)) return;

    ScopedConn c(srv);
    ASSERT(ctx, c.ok(), "connect failed");
    int fd = c.fd();

    // 构造一簇等字符和名字："CH_ECS_" + a + (155-a)，a∈['A','Z']；前缀和恒定、后两字符和恒=155。
    //   → 全簇 hash1/hash2 相同 → 共享同一条探测链（loc, loc+c, loc+2c, ...）。
    std::vector<std::string> cluster;
    for (int a = 'A'; a <= 'Z'; a++) {
        char nm[GPLAT_TAGNAME_SIZE];
        snprintf(nm, sizeof(nm), "CH_ECS_%c%c", a, 155 - a);
        cluster.push_back(nm);
    }
    // 自检：全簇探测序列确实一致（同 hash1 起点、同 hash2 步长）。
    int h1 = board_hash1(cluster[0].c_str()), h2 = board_hash2(cluster[0].c_str());
    for (const auto& n : cluster) {
        CHECK(ctx, board_hash1(n.c_str()) == h1 && board_hash2(n.c_str()) == h2,
              "cluster name '%s' hash(%d,%d) != (%d,%d)", n.c_str(),
              board_hash1(n.c_str()), board_hash2(n.c_str()), h1, h2);
    }

    Rng rng(ctx.seed);
    ChurnModel m;

    // 建全簇 → 它们沿同一探测链依次落位。全量校验：每个都必须探测可达（inv4）。
    for (const auto& n : cluster)
        churn_create(ctx, fd, m, n, rng.range(16, 256), rng.range(1, 40), rng);
    if (!verify_board(ctx, fd, bf, m, base.live, "cluster: all created")) return;

    // 删奇数位（在链中间制造墓碑）→ 幸存者仍须可达（墓碑不截断同链探测）。
    for (size_t i = 1; i < cluster.size(); i += 2)
        churn_delete(ctx, fd, m, cluster[i]);
    if (!verify_board(ctx, fd, bf, m, base.live, "cluster: odd deleted (tombstones interleaved)")) return;

    // 重建被删者（墓碑复用路径，沿同链）→ 仍须全可达、数据正确。
    for (size_t i = 1; i < cluster.size(); i += 2)
        churn_create(ctx, fd, m, cluster[i], rng.range(16, 256), rng.range(1, 40), rng);
    if (!verify_board(ctx, fd, bf, m, base.live, "cluster: refilled")) return;

    // 删空 → 回基线。
    std::vector<std::string> all = m.names;
    for (const auto& n : all) churn_delete(ctx, fd, m, n);
    BoardInspector insp(bf);
    ASSERT(ctx, insp.ok(), "final inspector: %s", insp.error().c_str());
    BoardInvariantReport r = insp.validate();
    for (const auto& v : r.violations) TS_FAILF(ctx, "cluster final: %s", v.c_str());
    CHECK(ctx, r.liveCount == base.live && r.nextpos == base.nextpos,
          "cluster final not back to baseline (live %d/%d nextpos %d/%d)",
          r.liveCount, base.live, r.nextpos, base.nextpos);
}

// ===========================================================================
// [B] 删空再建满 —— 验数据区/类型区能完全回缩到 fixture 基线，再从基线重建
// ===========================================================================
TEST("board_churn.delete_to_empty_refill", TAG_CHURN | TAG_WHITEBOX)
{
    PRIVATE_SERVER(srv);
    const std::string bf = board_file(srv);
    Baseline base;
    if (!read_baseline(ctx, bf, base)) return;

    ScopedConn c(srv);
    ASSERT(ctx, c.ok(), "connect failed");
    int fd = c.fd();
    Rng rng(ctx.seed);

    auto fill_and_check = [&](int n, const char* phase) {
        ChurnModel m;
        for (int i = 0; i < n; i++) {
            char nm[GPLAT_TAGNAME_SIZE];
            snprintf(nm, sizeof(nm), "CH_%s_%04d", phase, i);
            churn_create(ctx, fd, m, nm, rng.range(16, 512), rng.range(1, 40), rng);
        }
        char w[48]; snprintf(w, sizeof(w), "%s: filled %d", phase, n);
        verify_board(ctx, fd, bf, m, base.live, w);

        std::vector<std::string> all = m.names;
        for (const auto& nm : all) churn_delete(ctx, fd, m, nm);

        BoardInspector insp(bf);
        ASSERT(ctx, insp.ok(), "%s: inspector: %s", phase, insp.error().c_str());
        BoardInvariantReport r = insp.validate();
        for (const auto& v : r.violations) TS_FAILF(ctx, "%s empty: %s", phase, v.c_str());
        CHECK(ctx, r.liveCount == base.live, "%s: liveCount=%d != baseline %d", phase, r.liveCount, base.live);
        CHECK(ctx, r.nextpos == base.nextpos, "%s: nextpos=%d != baseline %d", phase, r.nextpos, base.nextpos);
        CHECK(ctx, r.nexttypepos == base.nexttypepos,
              "%s: nexttypepos=%d != baseline %d", phase, r.nexttypepos, base.nexttypepos);
    };

    // 建满 60 → 全删回基线 → 再建满 60（不同名）→ 再全删回基线。
    fill_and_check(60, "A");
    fill_and_check(60, "B");
}

// ===========================================================================
// [B] 墓碑复用 —— 交替建删同名 tag：同名 create 必落回它自己的墓碑槽（不泄漏空槽）
// ===========================================================================
TEST("board_churn.tombstone_reuse", TAG_CHURN | TAG_WHITEBOX)
{
    PRIVATE_SERVER(srv);
    const std::string bf = board_file(srv);
    Baseline base;
    if (!read_baseline(ctx, bf, base)) return;

    ScopedConn c(srv);
    ASSERT(ctx, c.ok(), "connect failed");
    int fd = c.fd();
    Rng rng(ctx.seed);

    const char* NAME = "CH_TOMB_REUSE";
    const int CYCLES = 200 * ctx.scale;

    for (int i = 0; i < CYCLES; i++) {
        ChurnModel m;  // 单 tag 的临时影子
        if (!churn_create(ctx, fd, m, NAME, rng.range(16, 128), rng.range(1, 40), rng)) break;

        // 建后：可达、数据正确；存活数 = 基线+1。
        BoardInspector i1(bf);
        ASSERT(ctx, i1.ok(), "cycle %d inspector(after create): %s", i, i1.error().c_str());
        BoardInvariantReport r1 = i1.validate();
        if (!r1.ok) { for (auto& v : r1.violations) TS_FAILF(ctx, "cycle %d create: %s", i, v.c_str()); break; }
        if (r1.liveCount != base.live + 1) {
            TS_FAILF(ctx, "cycle %d: liveCount=%d != baseline+1(%d)", i, r1.liveCount, base.live + 1);
            break;
        }

        if (!churn_delete(ctx, fd, m, NAME)) break;
    }

    // 关键断言：交替 200 轮后，空槽至多消耗 1 个（墓碑被复用），绝非每轮消耗一个。
    BoardInspector insp(bf);
    ASSERT(ctx, insp.ok(), "final inspector: %s", insp.error().c_str());
    BoardInvariantReport r = insp.validate();
    for (const auto& v : r.violations) TS_FAILF(ctx, "tombstone_reuse final: %s", v.c_str());
    CHECK(ctx, r.liveCount == base.live, "final liveCount=%d != baseline %d", r.liveCount, base.live);
    // 复用成立 ⇒ 最终空槽 == 基线空槽-1（恰一个槽长期充当该名的 create/墓碑位）。
    CHECK(ctx, r.emptySlots == base.empty - 1,
          "tombstone NOT reused: emptySlots=%d, expected baseline-1=%d after %d create/delete cycles "
          "(每轮泄漏一个空槽 → 墓碑未被同名 create 复用)",
          r.emptySlots, base.empty - 1, CYCLES);
    printf("    [info] after %d create/delete cycles of same name: empty %d→%d (消耗 %d 个槽), tomb=%d\n",
           CYCLES, base.empty, r.emptySlots, base.empty - r.emptySlots, r.tombstones);
}

// ===========================================================================
// [B1 已修复][destructive] 并发建删 + 读 —— DeleteItem 持全部条带锁后 memmove，读写不再撕裂
// ===========================================================================
// 布局：POOL 个 256B tag，分区给 Nmut 个 mutator 线程独占（各自建/删自己那份，无跨线程协调）；
//   Nread 个 reader 线程随机 readb 任一 tag，对读回字节做 **seq 无关** 的 check_payload：
//   - 返回 OK 且 payload 完好 → 正常；
//   - 返回 TAG_NOT_EXIST（恰被并发删）→ 跳过（非错误）；
//   - 返回 OK 但 payload 自校验失败 → **撕裂读**（命中 B1）。
// 每次 delete 触发 ~8MB memmove（覆盖整个数据区）→ 并发 readb 极易读到搬移中途的字节。
// 预期：torn==0 且服务端存活。
TEST("board_churn.concurrent_delete_race", TAG_CHURN | TAG_DESTRUCTIVE)
{
    PRIVATE_SERVER_T(srv, 4);  // ≥2 worker 线程才能让 delete 与 read 真正并发
    const std::string bf = board_file(srv);

    const int POOL    = 16;              // tag 池大小
    const int NMUT    = 2;               // mutator 线程（建/删）
    const int NREAD   = 4;               // reader 线程（读 + 自校验）
    const int RUN_MS  = 1500 * ctx.scale;
    const int TAGSZ   = 256;

    // 预建整个池（各 tag 一份固定 self-checking payload），建立初始数据面。
    auto tag_name = [](int i) { char b[GPLAT_TAGNAME_SIZE]; snprintf(b, sizeof(b), "CH_RACE_%03d", i); return std::string(b); };
    {
        ScopedConn c(srv);
        ASSERT(ctx, c.ok(), "seed connect failed");
        Rng rng(ctx.seed);
        for (int i = 0; i < POOL; i++) {
            unsigned err = 0;
            char ty = 0x7;
            if (!createtag(c.fd(), tag_name(i).c_str(), TAGSZ, &ty, 1, &err)) {
                ASSERT(ctx, false, "seed createtag(%d) failed: err=%u(%s)", i, err, err_name(err));
            }
            std::vector<char> p; make_payload(p, TAGSZ, (uint32_t)i, rng);
            REQUIRE_OK(ctx, writeb(c.fd(), tag_name(i).c_str(), p.data(), TAGSZ, &err), err);
        }
    }

    std::atomic<bool>     stop{false};
    std::atomic<long>     torn{0};        // 撕裂读次数（B1 的实锤）
    std::atomic<long>     reads{0};       // 成功读次数（分母）
    std::atomic<long>     mutations{0};   // 建/删次数
    std::atomic<bool>     netError{false};

    // mutator：独占 [lo,hi) 子区间，交替删/建自己的 tag（8MB memmove 源）。
    auto mutator = [&](int lo, int hi, uint64_t seed) {
        int fd = connectgplat(srv.ip(), srv.port());
        if (fd < 0) { netError = true; return; }
        Rng rng(seed);
        std::vector<bool> present(hi - lo, true);
        while (!stop.load(std::memory_order_relaxed)) {
            int idx = rng.range(lo, hi - 1);
            unsigned err = 0;
            if (present[idx - lo]) {
                if (deletetag(fd, tag_name(idx).c_str(), &err)) present[idx - lo] = false;
            } else {
                char ty = 0x7;
                if (createtag(fd, tag_name(idx).c_str(), TAGSZ, &ty, 1, &err)) {
                    std::vector<char> p; make_payload(p, TAGSZ, (uint32_t)idx, rng);
                    writeb(fd, tag_name(idx).c_str(), p.data(), TAGSZ, &err);
                    present[idx - lo] = true;
                }
            }
            mutations.fetch_add(1, std::memory_order_relaxed);
        }
        disconnectgplat(fd);
    };

    // reader：随机读任一 tag，做 seq 无关的完整性校验。
    auto reader = [&](uint64_t seed) {
        int fd = connectgplat(srv.ip(), srv.port());
        if (fd < 0) { netError = true; return; }
        Rng rng(seed);
        std::vector<char> buf(TAGSZ, 0);
        while (!stop.load(std::memory_order_relaxed)) {
            int idx = rng.range(0, POOL - 1);
            unsigned err = 0;
            if (readb(fd, tag_name(idx).c_str(), buf.data(), TAGSZ, &err, nullptr)) {
                reads.fetch_add(1, std::memory_order_relaxed);
                // createtag 与首次 writeb 之间 tag 全零，属合法中间态而非撕裂
                bool allZero = std::all_of(buf.begin(), buf.end(), [](char c) { return c == 0; });
                std::string pe;
                if (!allZero && !check_payload(buf.data(), TAGSZ, -1, &pe))
                    torn.fetch_add(1, std::memory_order_relaxed);  // 撕裂读
            }
            // readb 失败多为 TAG_NOT_EXIST（并发删）——非错误，忽略。
        }
        disconnectgplat(fd);
    };

    std::vector<std::thread> pool;
    int per = POOL / NMUT;
    for (int t = 0; t < NMUT; t++)
        pool.emplace_back(mutator, t * per, (t == NMUT - 1) ? POOL : (t + 1) * per, ctx.seed ^ (0x9E37u * (t + 1)));
    for (int t = 0; t < NREAD; t++)
        pool.emplace_back(reader, ctx.seed ^ (0xABCDu * (t + 101)));

    std::this_thread::sleep_for(std::chrono::milliseconds(RUN_MS));
    stop.store(true);
    for (auto& th : pool) th.join();

    printf("    [info] race: %ld reads, %ld mutations, %ld torn reads; server alive=%d\n",
           reads.load(), mutations.load(), torn.load(), (int)srv.running());

    // B1 回归：出现撕裂读或服务端在竞态中死亡即失败。
    CHECK(ctx, torn.load() == 0,
          "B1: %ld torn reads out of %ld (DeleteItem 的 memmove 不持条带锁，与 readb 竞态撕裂数据)",
          torn.load(), reads.load());
    CHECK(ctx, srv.running(), "B1: server died during concurrent create/delete race");
    CHECK(ctx, !netError.load(), "B1: client connection setup failed during race");
}
