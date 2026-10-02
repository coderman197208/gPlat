// fixtures.h —— 测试夹具目录（tag/queue 的单一事实源）+ 本地 API 建夹具入口
//
// 设计：所有“预建” tag/queue 的名字与尺寸集中在 ts::fx 命名空间，
//   既被 fixtures.cpp（用本地 API 建文件）引用，也被各 cases 引用，避免两处写死不一致。
//   create_all_fixtures() 用 higplat 的“本地 API”（直接 mmap QBD 文件）在给定 qbdfile 目录下建好一切；
//   它会改本进程的全局 dataQuePath 与“已加载表”，因此必须在 forked 子进程或独立 gen_fixtures 进程里调用。
#pragma once

namespace ts {
namespace fx {

// ---- 共享 BOARD ----
constexpr const char* BOARD        = "BOARD";
constexpr int         BOARD_SIZE   = 8 * 1024 * 1024;  // 数据区字节数（足够功能用例 + 适度 churn）

// 标量/数组 tag（带类型描述符，供 readtype/类型显示用例）
constexpr const char* TAG_BOOL     = "T_BOOL";          // Boolean, 1B
constexpr const char* TAG_I16      = "T_I16";           // Int16,  2B
constexpr const char* TAG_I32      = "T_I32";           // Int32,  4B
constexpr const char* TAG_I64      = "T_I64";           // Int64,  8B
constexpr const char* TAG_F32      = "T_F32";           // Single, 4B
constexpr const char* TAG_F64      = "T_F64";           // Double, 8B
constexpr const char* TAG_ARR      = "T_ARR_F32_4";     // Single[4], 16B

// 通用二进制 tag（char[N]），供写读/pub-sub/发送路径等
constexpr const char* TAG_BIN256   = "T_BIN256";        // char[256]
constexpr const char* TAG_BIN4K    = "T_BIN4K";         // char[4096]
constexpr const char* TAG_BINMAX   = "T_BINMAX";        // char[16384]（满包边界）
constexpr const char* TAG_POST     = "T_POST";          // char[256]，订阅投递用

// 字符串 tag（String 类型，容量 = itemsize）
constexpr const char* STR_1        = "S_CAP1";          // itemsize 1（恰满/溢出一字节）
constexpr const char* STR_16       = "S_CAP16";         // itemsize 16
constexpr const char* STR_64       = "S_CAP64";         // itemsize 64
constexpr const char* STR_MAX      = "S_CAPMAX";        // itemsize 16384（理论上限）

// 请求/响应用 tag（getresponse）
constexpr const char* REQ_TAG      = "REQ_A";           // char[256]
constexpr const char* RSP_TAG      = "RSP_A";           // char[256]

// 持久化专用 tag（重启后应仍在）
constexpr const char* TAG_PERSIST  = "T_PERSIST";       // Int32, 4B

// ---- 队列：NORMAL/SHIFT × BINARY/ASCII ----
constexpr const char* Q_NORMAL_BIN = "Q_NB";
constexpr const char* Q_SHIFT_BIN  = "Q_SB";
constexpr const char* Q_NORMAL_ASC = "Q_NA";
constexpr const char* Q_SHIFT_ASC  = "Q_SA";
constexpr const char* Q_BIG_BIN    = "Q_BIG";           // recsize 2048（边界）
constexpr int         Q_RECSIZE    = 64;
constexpr int         Q_COUNT      = 17;                // 容量 = COUNT-1 = 16
constexpr int         Q_BIG_RECSIZE = 2048;
constexpr int         Q_BIG_COUNT   = 5;

}  // namespace fx

// 在 qbdPath 目录下建好全部标准夹具（本地 API）。返回失败项数（0 = 全部成功）。
// 必须在 forked 子进程 / 独立进程里调用（会污染本进程全局态）。
int create_all_fixtures(const char* qbdPath, int scale);

}  // namespace ts
