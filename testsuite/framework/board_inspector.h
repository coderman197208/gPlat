// board_inspector.h —— BOARD 文件白盒结构校验器（方案 §6）
//
// 目的：频繁建删 tag 后，直接只读 mmap sandbox 的 BOARD 文件，按服务端
//   BOARD_HEAD / BOARD_INDEX_STRUCT 的内存布局复算结构不变量，独立于网络 API
//   证明哈希索引 + 数据区 + 类型区在 CreateItem/DeleteItem 搬移后仍自洽。
//
// 为什么不直接 include 服务端私有头 higplat/qbd.h：
//   qbd.h 是服务端/本地库的实现细节（且会拉入一堆无关声明）。本校验器改为定义
//   **与之二进制兼容的镜像结构**（board_inspector.cpp 内，#pragma pack(push,8) +
//   std::mutex 成员占位），只读 POD 字段、**绝不触碰 mutex 字节**。镜像与服务端用
//   同一工具链编译 → sizeof/偏移自动一致；另有多道运行时自检兜底布局漂移。
//
// 并发纪律：本校验器假定 BOARD 处于**静止**态（无并发写）。用例应在“本轮所有
//   createtag/deletetag/writeb 的响应都已返回”之后再调用 validate()——否则会与
//   服务端的 DeleteItem memmove 竞争读到中间态（这正是缺陷 B1 的温床，另案覆盖）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>    // timespec
#include <string>
#include <vector>

namespace ts {

// 复刻服务端常量（higplat/qbd.h）。上游若改，这里需同步；validate() 有结构自检兜底。
namespace board_layout {
constexpr int MAXDQNAMELENTH   = 40;                        // tag 名上限（含 '\0'）
constexpr int INDEXSIZE        = 7177;                      // 哈希槽数（质数）
constexpr int TABLESIZE        = 277;                       // hash1 的模（质数）
constexpr int TYPEAVGSIZE      = 32;                        // 类型区按此均值预留
constexpr int TYPEMAXSIZE      = 2048;                      // 单个类型描述符上限
constexpr int BOARD_T          = 1;                         // enum { QUEUE_T=0, BOARD_T=1 }
constexpr int TYPE_REGION_SIZE = INDEXSIZE * TYPEAVGSIZE;   // 类型区总字节 = 229664
}  // namespace board_layout

// 与 higplat/qbd.h 的 hash1/hash2 逐字节一致（按字符和取模）。探测可达性校验需复算。
int board_hash1(const char* s);
int board_hash2(const char* s);

// 单条存活 tag 的白盒快照。
struct BoardItem {
    std::string name;
    int      slot     = -1;   // 在 index[] 中的下标
    int      startpos = 0;    // 数据区内偏移（相对数据区基址）
    int      itemsize = 0;    // 数据字节数
    int      strlenth = 0;    // 字符串当前长度（二进制 tag 无意义）
    int      typeaddr = 0;    // 类型区内偏移
    int      typesize = 0;    // 类型描述符字节数（0 = 无类型）
    timespec timestamp{};     // 最后写入时间
};

// 结构不变量校验报告（方案 §6 的六类）。
struct BoardInvariantReport {
    bool ok          = false;   // layoutValid && violations.empty()
    bool layoutValid = false;   // 文件/镜像布局自检通过（否则下方字段不可信）

    // —— 头部读数（直接取自 BOARD_HEAD 前部整型，布局无关，始终可信）——
    int qbdtype     = -1;
    int totalsize   = -1;       // 头+数据区（不含类型区）
    int typesizeFld = -1;       // 头字段 typesize（类型区总大小）
    int nextpos     = -1;       // 数据区 bump 指针
    int nexttypepos = -1;       // 类型区 bump 指针
    int remain      = -1;       // 数据区剩余
    int typeremain  = -1;       // 类型区剩余
    int indexcount  = -1;       // 头记录的存活槽数

    // —— 实测统计 ——
    int liveCount   = -1;       // 实扫存活槽数（name 非空 && !erased）
    int tombstones  = -1;       // 墓碑数（name 非空 && erased）
    int emptySlots  = -1;       // 空槽数（name 为空）
    long dataUsed   = -1;       // 存活 tag itemsize 之和（应==nextpos）
    long typeUsed   = -1;       // 存活 tag typesize 之和（应==nexttypepos）

    // 逼近死锁预警：INDEXSIZE-1 - (live+tomb)。<=0 时 find 探测对缺失名会死循环。
    int probeSlack  = -1;

    std::vector<std::string> violations;  // 每条不变量违背的人读描述（空=全过）
};

// 只读 mmap 一个 BOARD 文件，做白盒结构校验。只读 POD、绝不触碰 mutex 字节。
// 非线程安全；非拷贝。生命周期内持有 mmap 映射，析构时 munmap。
class BoardInspector {
public:
    // 打开并 mmap <path>（通常是 <sandbox>/qbdfile/BOARD）。
    // 失败或布局自检不过 → ok()==false，error() 给原因。
    explicit BoardInspector(const std::string& boardFilePath);
    ~BoardInspector();

    BoardInspector(const BoardInspector&)            = delete;
    BoardInspector& operator=(const BoardInspector&) = delete;

    bool               ok() const { return map_ != nullptr && layoutOk_; }
    const std::string& error() const { return err_; }

    // 全量不变量校验（方案 §6 六类 + 头/实测一致性）。
    BoardInvariantReport validate() const;

    // 枚举所有存活 tag（按 slot 升序）。
    std::vector<BoardItem>   live_items() const;
    std::vector<std::string> live_names() const;

    // 读某存活 tag 的原始数据字节（itemsize 长）。未找到→false。
    // 供影子模型比对：验证 DeleteItem 的 memmove 没有损坏邻接 tag 数据。
    bool read_item_bytes(const std::string& name, std::vector<char>& out) const;

    // 读某存活 tag 的类型描述符字节（typesize 长，可能为 0）。未找到→false。
    bool read_type_bytes(const std::string& name, std::vector<char>& out) const;

    // 用 hash1/hash2 探测序列定位 name（复刻服务端 find 探测，跳过墓碑、遇空槽即止）。
    // 返回命中的 slot；-1=探测到空槽仍未匹配（不可达）或步数越界。
    int probe_find(const std::string& name) const;

    // 暴露给框架自检：镜像推导的头大小 / 数据区基址偏移（= 数据 tag 的 base）。
    size_t head_size() const;

private:
    // 返回某 slot 的字段（读 POD，不碰 mutex）。调用方保证 0<=slot<INDEXSIZE。
    bool slot_is_live(int slot) const;
    bool slot_is_empty(int slot) const;
    const char* slot_name(int slot) const;
    void read_slot(int slot, BoardItem& out) const;

    void*       map_      = nullptr;  // mmap 基址（只读）
    size_t      mapSize_  = 0;        // 文件/映射字节数
    bool        layoutOk_ = false;    // 构造期布局自检结果
    std::string err_;
};

}  // namespace ts
