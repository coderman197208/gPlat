// board_inspector.cpp —— 见 board_inspector.h
//
// 布局镜像策略：在匿名命名空间内定义与服务端 BOARD_HEAD / BOARD_INDEX_STRUCT
//   **二进制兼容**的镜像（同 #pragma pack(push,8)、同字段序、std::mutex 成员占位）。
//   我们只 reinterpret_cast mmap 指针并读 POD 字段——**从不构造镜像对象、从不读写
//   mutex 字节**（那些字节在被测进程里是活锁，于本进程只作对齐占位）。
//   同一工具链编译 → 镜像 sizeof/偏移与服务端一致。另有运行时自检兜底。
#include "board_inspector.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <set>

namespace ts {

using namespace board_layout;

// ===========================================================================
// hash1 / hash2 —— 与 higplat/qbd.h 逐字节一致（字符和取模）
// ===========================================================================
int board_hash1(const char* s)
{
    int k = 0;
    while (*s) { k += (int)*s; s++; }   // 注意：与服务端一样用 (int)*s（char 在 x86-64 为有符号）
    return k % TABLESIZE;               // ASCII 名 → [0,276]
}

int board_hash2(const char* s)
{
    int k = 0, remain;
    while (*s) { k += (int)*s; s++; }
    remain = k % (TABLESIZE - 2);       // % 275
    return remain == 0 ? 1 : remain;    // ASCII 名 → [1,274]
}

namespace {

// ---------------------------------------------------------------------------
// 布局镜像（与 higplat/qbd.h 对齐）。pack(8)、字段序完全一致。
// ---------------------------------------------------------------------------
#pragma pack(push, 8)

struct BoardIndexMirror {
    char     itemname[MAXDQNAMELENTH];  // @0  (40)
    int      startpos;                  // @40
    int      itemsize;                  // @44
    int      strlenth;                  // @48
    bool     erased;                    // @52
    timespec timestamp;                 // @56 (16, 8 对齐 → 前面 bool 补 3 字节)
    int      typeaddr;                  // @72
    int      typesize;                  // @76
};                                      // sizeof == 80

struct BoardHeadMirror {
    int        qbdtype;                 // @0
    int        counter;                 // @4
    int        totalsize;               // @8
    int        typesize;                // @12
    int        nextpos;                 // @16
    int        nexttypepos;             // @20
    int        remain;                  // @24
    int        typeremain;              // @28
    int        indexcount;              // @32  (之后补 4 字节对齐到 8)
    std::mutex mutex_rw;                // @40  —— 仅占位，绝不访问
    std::mutex mutex_rw_tag[64];        // @80  —— 仅占位，绝不访问
    BoardIndexMirror index[INDEXSIZE];  // @2640
};

#pragma pack(pop)

// pack(8) 下这些是确定的，与 std::mutex 大小无关 —— 验证字段序/我们对头部整型的直接下标假设。
static_assert(sizeof(BoardIndexMirror) == 80, "BOARD_INDEX_STRUCT 镜像大小应为 80");
static_assert(offsetof(BoardIndexMirror, startpos) == 40, "startpos 偏移");
static_assert(offsetof(BoardIndexMirror, erased)   == 52, "erased 偏移");
static_assert(offsetof(BoardIndexMirror, timestamp) == 56, "timestamp 偏移");
static_assert(offsetof(BoardIndexMirror, typeaddr) == 72, "typeaddr 偏移");
static_assert(offsetof(BoardHeadMirror, qbdtype)   == 0,  "头首字段");
static_assert(offsetof(BoardHeadMirror, totalsize) == 8,  "totalsize 偏移（头整型直接下标依赖）");
static_assert(offsetof(BoardHeadMirror, indexcount) == 32, "indexcount 偏移");

// 头部整型用直接下标读（布局无关、绝对可信：9 个 int 从偏移 0 紧排）。
// [0]qbdtype [1]counter [2]totalsize [3]typesize [4]nextpos
// [5]nexttypepos [6]remain [7]typeremain [8]indexcount
enum HeadInt {
    HI_QBDTYPE = 0, HI_COUNTER, HI_TOTALSIZE, HI_TYPESIZE, HI_NEXTPOS,
    HI_NEXTTYPEPOS, HI_REMAIN, HI_TYPEREMAIN, HI_INDEXCOUNT
};

}  // namespace

// ===========================================================================
// 内部访问器
// ===========================================================================
static inline const BoardHeadMirror* as_head(const void* m)
{
    return reinterpret_cast<const BoardHeadMirror*>(m);
}
static inline int head_int(const void* m, HeadInt which)
{
    return reinterpret_cast<const int32_t*>(m)[which];
}

size_t BoardInspector::head_size() const { return sizeof(BoardHeadMirror); }

const char* BoardInspector::slot_name(int slot) const
{
    return as_head(map_)->index[slot].itemname;
}
bool BoardInspector::slot_is_empty(int slot) const
{
    return slot_name(slot)[0] == '\0';
}
bool BoardInspector::slot_is_live(int slot) const
{
    const BoardIndexMirror& ix = as_head(map_)->index[slot];
    return ix.itemname[0] != '\0' && !ix.erased;
}
void BoardInspector::read_slot(int slot, BoardItem& out) const
{
    const BoardIndexMirror& ix = as_head(map_)->index[slot];
    out.slot = slot;
    // itemname 可能恰好占满 40 字节无 '\0'（CreateItem 用 strcpy，实际名 <40 恒有终止符）；
    // 仍按定长安全拷贝，避免越界。
    char nm[MAXDQNAMELENTH + 1];
    memcpy(nm, ix.itemname, MAXDQNAMELENTH);
    nm[MAXDQNAMELENTH] = '\0';
    out.name     = nm;
    out.startpos = ix.startpos;
    out.itemsize = ix.itemsize;
    out.strlenth = ix.strlenth;
    out.typeaddr = ix.typeaddr;
    out.typesize = ix.typesize;
    out.timestamp = ix.timestamp;
}

// ===========================================================================
// 构造 / 析构：mmap + 布局自检
// ===========================================================================
BoardInspector::BoardInspector(const std::string& boardFilePath)
{
    int fd = ::open(boardFilePath.c_str(), O_RDONLY);
    if (fd < 0) {
        err_ = "open(" + boardFilePath + ") failed: " + std::strerror(errno);
        return;
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        err_ = "fstat failed: " + std::string(std::strerror(errno));
        ::close(fd);
        return;
    }
    mapSize_ = (size_t)st.st_size;

    // 文件至少要能装下头（含 index[]）。
    if (mapSize_ < sizeof(BoardHeadMirror)) {
        char buf[128];
        snprintf(buf, sizeof(buf), "file too small: %zu < head %zu", mapSize_, sizeof(BoardHeadMirror));
        err_ = buf;
        ::close(fd);
        return;
    }

    void* m = ::mmap(nullptr, mapSize_, PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);  // mmap 后 fd 可关
    if (m == MAP_FAILED) {
        err_ = "mmap failed: " + std::string(std::strerror(errno));
        map_ = nullptr;
        return;
    }
    map_ = m;

    // —— 布局自检：这些关系对任何健康 BOARD 恒成立，且不依赖 mutex sizeof ——
    const int qbdtype   = head_int(map_, HI_QBDTYPE);
    const int totalsize = head_int(map_, HI_TOTALSIZE);
    const int typesize  = head_int(map_, HI_TYPESIZE);

    if (qbdtype != BOARD_T) {
        char buf[96];
        snprintf(buf, sizeof(buf), "qbdtype=%d != BOARD_T(%d): not a BOARD file", qbdtype, BOARD_T);
        err_ = buf;
        return;  // layoutOk_ 仍 false
    }
    // CreateB：fileSize = sizeof(BOARD_HEAD)+size+TYPE_REGION_SIZE；
    //          totalsize = sizeof(BOARD_HEAD)+size；typesize = TYPE_REGION_SIZE。
    // 故恒有 fileSize == totalsize + typesize，且 typesize == TYPE_REGION_SIZE。
    if (typesize != TYPE_REGION_SIZE) {
        char buf[96];
        snprintf(buf, sizeof(buf), "typesize=%d != TYPE_REGION_SIZE(%d)", typesize, TYPE_REGION_SIZE);
        err_ = buf;
        return;
    }
    if ((size_t)totalsize + (size_t)typesize != mapSize_) {
        char buf[128];
        snprintf(buf, sizeof(buf), "totalsize(%d)+typesize(%d)=%zu != fileSize %zu",
                 totalsize, typesize, (size_t)totalsize + (size_t)typesize, mapSize_);
        err_ = buf;
        return;
    }
    // 头大小（镜像推导）必须 <= totalsize（数据区非负）。
    if (sizeof(BoardHeadMirror) > (size_t)totalsize) {
        char buf[128];
        snprintf(buf, sizeof(buf), "mirror head %zu > totalsize %d (layout drift?)",
                 sizeof(BoardHeadMirror), totalsize);
        err_ = buf;
        return;
    }
    layoutOk_ = true;
}

BoardInspector::~BoardInspector()
{
    if (map_ != nullptr) { ::munmap(map_, mapSize_); map_ = nullptr; }
}

// ===========================================================================
// 枚举存活 tag
// ===========================================================================
std::vector<BoardItem> BoardInspector::live_items() const
{
    std::vector<BoardItem> v;
    if (!ok()) return v;
    for (int i = 0; i < INDEXSIZE; i++) {
        if (slot_is_live(i)) {
            BoardItem it;
            read_slot(i, it);
            v.push_back(std::move(it));
        }
    }
    return v;
}

std::vector<std::string> BoardInspector::live_names() const
{
    std::vector<std::string> v;
    for (const auto& it : live_items()) v.push_back(it.name);
    return v;
}

// ===========================================================================
// 探测定位（复刻服务端 find 探测：跳墓碑、遇空槽即止）
// ===========================================================================
int BoardInspector::probe_find(const std::string& name) const
{
    if (!ok()) return -1;
    int loc = board_hash1(name.c_str());
    int c   = board_hash2(name.c_str());
    // 服务端循环：while(itemname!="\0" && itemname!=name) loc=(loc+c)%INDEXSIZE;
    // 复刻之，并加步数上限防（镜像错位/损坏导致的）死循环。
    for (int steps = 0; steps <= INDEXSIZE; steps++) {
        const char* nm = slot_name(loc);
        if (nm[0] == '\0') return -1;                 // 空槽：未找到（探测到此截断）
        if (std::strncmp(nm, name.c_str(), MAXDQNAMELENTH) == 0) {
            // 命中名字；但只有 !erased 才算真正定位到存活 tag。
            return as_head(map_)->index[loc].erased ? -1 : loc;
        }
        loc = (loc + c) % INDEXSIZE;
    }
    return -1;  // 步数越界（表被墓碑/存活填满且无空槽）——退化
}

// ===========================================================================
// 读数据 / 类型字节
// ===========================================================================
bool BoardInspector::read_item_bytes(const std::string& name, std::vector<char>& out) const
{
    if (!ok()) return false;
    int slot = probe_find(name);
    if (slot < 0) return false;
    const BoardIndexMirror& ix = as_head(map_)->index[slot];
    const size_t base = sizeof(BoardHeadMirror);           // 数据区基址偏移
    if (ix.itemsize < 0) return false;
    size_t off = base + (size_t)ix.startpos;
    if (off + (size_t)ix.itemsize > mapSize_) return false;  // 越界保护
    out.assign((const char*)map_ + off, (const char*)map_ + off + ix.itemsize);
    return true;
}

bool BoardInspector::read_type_bytes(const std::string& name, std::vector<char>& out) const
{
    if (!ok()) return false;
    int slot = probe_find(name);
    if (slot < 0) return false;
    const BoardIndexMirror& ix = as_head(map_)->index[slot];
    out.clear();
    if (ix.typesize <= 0) return true;  // 无类型：返回空
    const int totalsize = head_int(map_, HI_TOTALSIZE);
    size_t off = (size_t)totalsize + (size_t)ix.typeaddr;   // 类型区基址 = totalsize
    if (off + (size_t)ix.typesize > mapSize_) return false;
    out.assign((const char*)map_ + off, (const char*)map_ + off + ix.typesize);
    return true;
}

// ===========================================================================
// 全量不变量校验
// ===========================================================================
BoardInvariantReport BoardInspector::validate() const
{
    BoardInvariantReport r;
    auto fail = [&](const std::string& msg) { r.violations.push_back(msg); };
    char buf[256];

    if (map_ == nullptr || !layoutOk_) {
        r.layoutValid = false;
        fail("layout self-check failed: " + (err_.empty() ? std::string("<no map>") : err_));
        return r;
    }
    r.layoutValid = true;

    // —— 头部读数 ——
    r.qbdtype     = head_int(map_, HI_QBDTYPE);
    r.totalsize   = head_int(map_, HI_TOTALSIZE);
    r.typesizeFld = head_int(map_, HI_TYPESIZE);
    r.nextpos     = head_int(map_, HI_NEXTPOS);
    r.nexttypepos = head_int(map_, HI_NEXTTYPEPOS);
    r.remain      = head_int(map_, HI_REMAIN);
    r.typeremain  = head_int(map_, HI_TYPEREMAIN);
    r.indexcount  = head_int(map_, HI_INDEXCOUNT);

    const long dataRegion = (long)r.totalsize - (long)sizeof(BoardHeadMirror);  // 数据区总容量
    const long typeRegion = TYPE_REGION_SIZE;

    // —— 扫描全表，分类 + 收集存活 tag ——
    std::vector<BoardItem> live;
    int tomb = 0, empty = 0;
    for (int i = 0; i < INDEXSIZE; i++) {
        const BoardIndexMirror& ix = as_head(map_)->index[i];
        if (ix.itemname[0] == '\0') { empty++; continue; }
        if (ix.erased)              { tomb++;  continue; }
        BoardItem it; read_slot(i, it); live.push_back(std::move(it));
    }
    r.liveCount  = (int)live.size();
    r.tombstones = tomb;
    r.emptySlots = empty;
    r.probeSlack = (INDEXSIZE - 1) - (r.liveCount + tomb);

    // (1) indexcount == 实扫存活数
    if (r.indexcount != r.liveCount) {
        snprintf(buf, sizeof(buf), "inv1 indexcount=%d != live slots=%d", r.indexcount, r.liveCount);
        fail(buf);
    }
    // 槽数守恒：live+tomb+empty == INDEXSIZE
    if (r.liveCount + tomb + empty != INDEXSIZE) {
        snprintf(buf, sizeof(buf), "slot accounting live+tomb+empty=%d != INDEXSIZE=%d",
                 r.liveCount + tomb + empty, INDEXSIZE);
        fail(buf);
    }
    // indexcount 合法范围
    if (r.indexcount < 0 || r.indexcount > INDEXSIZE - 1) {
        snprintf(buf, sizeof(buf), "indexcount=%d out of [0,%d]", r.indexcount, INDEXSIZE - 1);
        fail(buf);
    }

    // (5) 存活名无重复
    {
        std::set<std::string> names;
        for (const auto& it : live) {
            if (!names.insert(it.name).second) {
                snprintf(buf, sizeof(buf), "inv5 duplicate live name '%s'", it.name.c_str());
                fail(buf);
            }
        }
    }

    // (2) 数据区紧致：存活 (startpos,itemsize) 按 startpos 升序无缝平铺 [0,nextpos)
    {
        std::vector<const BoardItem*> byPos;
        for (const auto& it : live) byPos.push_back(&it);
        std::sort(byPos.begin(), byPos.end(),
                  [](const BoardItem* a, const BoardItem* b) { return a->startpos < b->startpos; });
        long cursor = 0, used = 0;
        bool compactBad = false;
        for (const BoardItem* it : byPos) {
            if (it->itemsize <= 0) {
                snprintf(buf, sizeof(buf), "inv2 tag '%s' itemsize=%d <=0", it->name.c_str(), it->itemsize);
                fail(buf); compactBad = true; continue;
            }
            if (it->startpos != cursor) {
                snprintf(buf, sizeof(buf),
                         "inv2 data not compact at tag '%s': startpos=%d expected %ld (gap/overlap)",
                         it->name.c_str(), it->startpos, cursor);
                fail(buf); compactBad = true;
            }
            if ((long)it->startpos + it->itemsize > dataRegion) {
                snprintf(buf, sizeof(buf), "inv2 tag '%s' [%d,+%d) exceeds data region %ld",
                         it->name.c_str(), it->startpos, it->itemsize, dataRegion);
                fail(buf); compactBad = true;
            }
            cursor = (long)it->startpos + it->itemsize;
            used  += it->itemsize;
        }
        r.dataUsed = used;
        if (!compactBad && cursor != r.nextpos) {
            snprintf(buf, sizeof(buf), "inv2 live data end=%ld != nextpos=%d", cursor, r.nextpos);
            fail(buf);
        }
        // remain 一致性：remain == dataRegion - nextpos。亦即镜像头大小与服务端一致的强校验。
        if (r.remain != (int)(dataRegion - r.nextpos)) {
            snprintf(buf, sizeof(buf),
                     "inv2 remain=%d != dataRegion(%ld)-nextpos(%d)=%ld "
                     "(数据区不守恒，或镜像头大小与服务端不一致)",
                     r.remain, dataRegion, r.nextpos, dataRegion - r.nextpos);
            fail(buf);
        }
    }

    // (3) 类型区紧致：存活且 typesize>0 的 (typeaddr,typesize) 升序平铺 [0,nexttypepos)
    {
        std::vector<const BoardItem*> byType;
        for (const auto& it : live) if (it.typesize > 0) byType.push_back(&it);
        std::sort(byType.begin(), byType.end(),
                  [](const BoardItem* a, const BoardItem* b) { return a->typeaddr < b->typeaddr; });
        long cursor = 0, used = 0;
        bool compactBad = false;
        for (const BoardItem* it : byType) {
            if (it->typeaddr != cursor) {
                snprintf(buf, sizeof(buf),
                         "inv3 type not compact at tag '%s': typeaddr=%d expected %ld",
                         it->name.c_str(), it->typeaddr, cursor);
                fail(buf); compactBad = true;
            }
            if ((long)it->typeaddr + it->typesize > typeRegion) {
                snprintf(buf, sizeof(buf), "inv3 tag '%s' type [%d,+%d) exceeds type region %ld",
                         it->name.c_str(), it->typeaddr, it->typesize, typeRegion);
                fail(buf); compactBad = true;
            }
            cursor = (long)it->typeaddr + it->typesize;
            used  += it->typesize;
        }
        r.typeUsed = used;
        if (!compactBad && cursor != r.nexttypepos) {
            snprintf(buf, sizeof(buf), "inv3 live type end=%ld != nexttypepos=%d", cursor, r.nexttypepos);
            fail(buf);
        }
        if (r.typeremain != (int)(typeRegion - r.nexttypepos)) {
            snprintf(buf, sizeof(buf), "inv3 typeremain=%d != typeRegion(%ld)-nexttypepos(%d)=%ld",
                     r.typeremain, typeRegion, r.nexttypepos, typeRegion - r.nexttypepos);
            fail(buf);
        }
    }

    // (4) 探测可达性：每个存活 tag 的 find 探测必须定位到自身 slot
    for (const auto& it : live) {
        int slot = probe_find(it.name);
        if (slot != it.slot) {
            snprintf(buf, sizeof(buf),
                     "inv4 tag '%s' unreachable: probe landed slot=%d, actual slot=%d "
                     "(墓碑截断探测链 or 哈希不一致)",
                     it.name.c_str(), slot, it.slot);
            fail(buf);
        }
    }

    // (6) 墓碑/空槽统计 + 死循环预警：空槽耗尽 → find 对缺失名会死循环
    if (r.emptySlots <= 0) {
        snprintf(buf, sizeof(buf),
                 "inv6 no empty slots left (live=%d tomb=%d): find-probe on a missing name would hang",
                 r.liveCount, tomb);
        fail(buf);
    }

    r.ok = r.violations.empty();
    return r;
}

}  // namespace ts
