// xfail_ids.h —— XFAIL 已知缺陷ID 的符号常量（与方案 §9 登记表、test_registry.cpp
// 的 kBugLabels 一一对应）。用例写 TEST_XFAIL(name, tags, BUG_P1) 而非裸数字，
// 运行器据 id→标签（"P1"）输出，便于对照登记表。
//
// ⚠️ 这些值必须与 test_registry.cpp 里 kBugLabels 的编号保持一致。
#pragma once

namespace ts {

enum XfailBug {
    BUG_C1 = 1,   // （已修复，保留编号）createtag typesize==0
    BUG_C2 = 2,   // （已修复，保留编号）createtag 负 tagsize：server memset 超大长度崩溃
    BUG_C3 = 3,   // （已修复，保留编号）createtag 负 typesize：客户端越界读 + 服务端流错位
    BUG_C4 = 4,   // （已修复，保留编号）writeb_string value==NULL：客户端 strlen(NULL) 崩溃
    BUG_C5 = 5,   // （已修复，保留编号）clearq error==NULL：空指针解引用崩溃
    BUG_C6 = 6,   // （已修复，保留编号）unblock_connect fd≥1024 FD_SET 栈溢出
    BUG_S1 = 7,   // （已修复，保留编号）bodysize 截断为 u16：body==65536 → 0 长派发
    BUG_S2 = 8,   // （已修复，保留编号）e_pkgLen>16384：丢头不关连接 → 流错位
    BUG_P1 = 9,   // （已修复，保留编号）HandleSubscribe 不校验 tag 存在
    BUG_P2 = 10,  // （已修复，保留编号）NotifySubscriber >500：server exit(1)
    BUG_P3 = 11,  // m_listPost 无界：只订不取 / 定时器订阅堆积
    BUG_G1 = 12,  // （已修复，保留编号）m_mapResponseOwner：deletetag 后残留；现由 ForgetDeletedTag 清理
    BUG_M1 = 13,  // 连接池：ngx_get_connection 无界增长、不缩
    BUG_B1 = 14,  // （已修复，保留编号）DeleteItem memmove：不持条带锁，与读写竞态
    BUG_L1 = 15,  // 致命启动失败（gplat_load_qbd 失败）以退出码 0 退出，应为 1
};

}  // namespace ts
