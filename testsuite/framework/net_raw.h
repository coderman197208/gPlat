// net_raw.h —— 裸 socket + 手工 MSGHEAD 收发
//
// 用途：
//   1) 协议模糊/破坏性用例（第 7.5/7.12 节）：手工构造 MSGHEAD，故意让 bodysize/datasize 错位、
//      发超长包、未知 MSGID 等，验证服务端的健壮性（应超时而非挂死、不崩溃或按登记表崩溃）。
//   2) Server 就绪探针：C API 的阻塞 recv 没有超时，若 worker 未就绪会挂死；这里用带 SO_RCVTIMEO
//      的裸连接做一次 READBOARDINFO 往返来判定“worker 已在处理请求”。
//
// 线协议（已核对 higplat.cpp）：请求 = MSGHEAD(180B，置 id/datasize/bodysize) + bodysize 字节 body；
//   响应同构。服务端按 header.bodysize 取 body。
#pragma once

#include <cstddef>
#include <ctime>   // msg.h 的 MSGHEAD 含 timespec，但其自身未包含 <time.h>
#include <vector>

#include "msg.h"  // MSGHEAD / MSGID / MAXMSGLEN（来自 include/）

namespace ts {

enum class RecvStatus { OK, TIMEOUT, CLOSED, PROTO_ERR };

// 建裸连接：设置 TCP_NODELAY 与 SO_RCVTIMEO(rcvTimeoutMs)；rcvbuf>0 时设 SO_RCVBUF，
// mss>0 时设 TCP_MAXSEG（配合小接收窗口制造部分发送）。失败返回 -1。
int raw_connect(const char* ip, int port, int rcvTimeoutMs = 2000, int rcvbuf = 0, int mss = 0);

// 构造一个清零的 MSGHEAD，填好 id/qname/itemname（qname 默认 "BOARD"）。
MSGHEAD make_head(int id, const char* qname = "BOARD", const char* itemname = "");

// send 全部 len 字节（MSG_NOSIGNAL）。返回是否成功。
bool raw_send(int fd, const void* buf, size_t len);

// recv 恰好 len 字节；超时/对端关闭/出错分别返回对应状态。
RecvStatus raw_recv(int fd, void* buf, size_t len);

// 发送一条消息：先发 header（原样，不改写 bodysize），再发 bodyLen 字节 body。
// 正常用法由调用方保证 h.bodysize==bodyLen；模糊用法可故意不一致。
bool send_msg(int fd, const MSGHEAD& h, const void* body = nullptr, int bodyLen = 0);

// 收一条完整消息：先收 header，再按 header.bodysize 收 body。
// bodysize 不在 [0, maxBody] 视为协议错（PROTO_ERR，通常表示流已错位）。
RecvStatus recv_msg(int fd, MSGHEAD& out, std::vector<char>& body, int maxBody = MAXMSGLEN);

}  // namespace ts
