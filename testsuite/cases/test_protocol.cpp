// test_protocol.cpp —— 裸协议模糊 / 破坏性（方案 §7.5 / §7.12；登记表 S1/S2）
//
// 这些用例手工拼 MSGHEAD（见 net_raw.h），故意制造畸形帧，验证服务端健壮性：
//   [N] unknown_msgid_dropped  —— 未注册 MSGID → 服务端静默丢包；客户端应**超时**而非挂死，
//                                 且服务端仍存活、仍能为其它连接正常服务。
//   [N] partial_header_abort   —— 只发半个包头随即 RST → 服务端不得崩溃，仍能服务。
//   [XFAIL S2] oversize_frame_desync —— bodysize 声明 > _PKG_MAX_LENGTH：服务端“丢头但不关连接”
//                                 （ngx_c_socket_request.cxx:204），随后客户端按协议发出的包体被
//                                 当作新包头解析 → **同一连接的流永久错位**。断言“正确行为”=该连接
//                                 后续的合法请求仍能得到响应（或连接被干净关闭），今日必错位 → XFAIL。
//   [XFAIL S1] bodysize_u16_truncation —— bodysize 字段在线协议是 int，但服务端用 unsigned short
//                                 读取（e_pkgLen = pPkgHeader->bodysize）。发 bodysize=65536 →
//                                 截断为 0 → 服务端按“无包体”立即派发，而客户端已按 65536 发出包体
//                                 → 同连接流错位。断言同上 → XFAIL。
//
// 隔离纪律：每个用例自建私有 ServerFixture（独立 sandbox + 自动空闲端口，绝不碰 8777）。
// 破坏性门控：全部标 TAG_PROTOCOL | TAG_DESTRUCTIVE，默认隐藏，仅 `make test-destructive` 运行。
#include <unistd.h>       // close
#include <sys/socket.h>   // setsockopt, SO_LINGER
#include <netinet/in.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "framework/ts.h"
#include "cases/xfail_ids.h"

using namespace ts;

namespace {

// 私有 server：threads>=2（并发接收），其余默认。
#define PRIVATE_PROTO_SERVER(SRV)                                               \
    ServerConfig SRV##_cfg;                                                     \
    SRV##_cfg.threads = 2;                                                      \
    SRV##_cfg.asan = ctx.expectAsan;                                           \
    ServerFixture SRV(SRV##_cfg);                                              \
    ASSERT(ctx, SRV.start(), "private proto fixture start failed: %s", SRV.startup_error().c_str())

// 在已连接的裸 fd 上做一次 READBOARDINFO 往返；返回是否收到结构正确的响应。
// 与 server_fixture 的就绪探针同构：响应须 id==READBOARDINFO、error==0、body==sizeof(BOARD_INFO)。
bool valid_roundtrip(int fd)
{
    MSGHEAD h = make_head(READBOARDINFO, "BOARD", "");
    h.datasize = sizeof(BOARD_INFO);
    h.bodysize = 0;
    if (!send_msg(fd, h)) return false;
    MSGHEAD out{};
    std::vector<char> body;
    RecvStatus rs = recv_msg(fd, out, body);
    return rs == RecvStatus::OK && out.id == READBOARDINFO && out.error == 0 &&
           body.size() == sizeof(BOARD_INFO);
}

// 新建一条裸连接并做一次合法往返，确认服务端整体仍健康。
bool fresh_server_ok(ServerFixture& srv)
{
    int fd = raw_connect(srv.ip(), srv.port(), 2000);
    if (fd < 0) return false;
    bool ok = valid_roundtrip(fd);
    close(fd);
    return ok;
}

}  // namespace

// ===========================================================================
// [N] 未注册 MSGID → 服务端静默丢包，客户端超时（非挂死），服务端存活
// ===========================================================================
TEST("protocol.unknown_msgid_dropped", TAG_PROTOCOL | TAG_DESTRUCTIVE)
{
    PRIVATE_PROTO_SERVER(srv);

    int fd = raw_connect(srv.ip(), srv.port(), 1000);  // 1s 接收超时作看门狗
    ASSERT(ctx, fd >= 0, "raw connect failed");

    // 构造一个“格式合法但 id 未注册”的包头（id=3 对应 statusHandler 的 noop 槽），无包体。
    MSGHEAD h = make_head(3, "BOARD", "");
    h.bodysize = 0;
    ASSERT(ctx, send_msg(fd, h), "send unknown-id header failed");

    // 服务端对 noop 不回包：客户端应在 SO_RCVTIMEO 内超时，而不是永久阻塞。
    MSGHEAD out{};
    std::vector<char> body;
    RecvStatus rs = recv_msg(fd, out, body);
    CHECK(ctx, rs == RecvStatus::TIMEOUT || rs == RecvStatus::CLOSED,
          "unknown MSGID should yield timeout/close, got status=%d", (int)rs);
    close(fd);

    // 关键健壮性：畸形请求不得影响服务端对其它连接的服务。
    CHECK(ctx, srv.running(), "server died after unknown MSGID");
    CHECK(ctx, fresh_server_ok(srv), "server no longer serves after unknown MSGID");
}

// ===========================================================================
// [N] 半个包头后 RST → 服务端收包状态机不得崩溃，仍能服务
// ===========================================================================
TEST("protocol.partial_header_abort", TAG_PROTOCOL | TAG_DESTRUCTIVE)
{
    PRIVATE_PROTO_SERVER(srv);

    // 连发若干“半包头 + RST”，压 _PKG_HD_RECVING 分支与异常关闭路径。
    for (int i = 0; i < 50; i++) {
        int fd = raw_connect(srv.ip(), srv.port(), 1000);
        ASSERT(ctx, fd >= 0, "raw connect #%d failed", i);
        MSGHEAD h = make_head(READBOARDINFO, "BOARD", "");
        h.bodysize = 0;
        // 只发 header 的前一半字节，制造“包头不完整”。
        int half = (int)sizeof(MSGHEAD) / 2;
        raw_send(fd, &h, half);
        // SO_LINGER{1,0} → close 立即 RST，让服务端在半包状态下读到连接异常。
        struct linger lg{1, 0};
        setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
        close(fd);
    }

    CHECK(ctx, srv.running(), "server died on partial-header + RST storm");
    CHECK(ctx, fresh_server_ok(srv), "server no longer serves after partial-header storm");
}

// ===========================================================================
// [XFAIL S2] bodysize > _PKG_MAX_LENGTH：丢头不关连接 → 同连接流错位
// ===========================================================================
TEST_XFAIL("protocol.oversize_frame_desync", TAG_PROTOCOL | TAG_DESTRUCTIVE, BUG_S2)
{
    PRIVATE_PROTO_SERVER(srv);

    int fd = raw_connect(srv.ip(), srv.port(), 1500);
    ASSERT(ctx, fd >= 0, "raw connect failed");

    // 声明一个超限包体（16385 > _PKG_MAX_LENGTH=MAXMSGLEN=16384），并按协议把这些“包体”字节真的发出去。
    // 服务端在 ngx_wait_request_handler_proc_p1 命中 e_pkgLen>_PKG_MAX_LENGTH：
    //   复位状态机、**不关连接**、也不消费这 16385 字节 → 这些字节被当作后续包头解析（流错位）。
    const int OVER = MAXMSGLEN + 1;  // 16385；16385 % 180 != 0 → 确定性错位
    MSGHEAD h = make_head(WRITEB, "BOARD", fx::TAG_BINMAX);
    h.datasize = OVER;
    h.bodysize = OVER;
    ASSERT(ctx, raw_send(fd, &h, sizeof(h)), "send oversize header failed");
    std::vector<char> blob(OVER, (char)0xFF);  // 0xFF 包体：被当包头时 bodysize=0xFFFF>MAX → 继续丢，稳定错位
    ASSERT(ctx, raw_send(fd, blob.data(), (int)blob.size()), "send oversize body failed");

    // 服务端存活（丢头不崩溃）——可靠断言。
    CHECK(ctx, srv.running(), "server died on oversize frame (unexpected: should just drop)");

    // 核心：同一连接后续的合法请求应仍可得到响应（或连接被干净关闭）。今日因流错位 → 超时。
    MSGHEAD h2 = make_head(READBOARDINFO, "BOARD", "");
    h2.datasize = sizeof(BOARD_INFO);
    h2.bodysize = 0;
    bool sent = send_msg(fd, h2);
    MSGHEAD out{};
    std::vector<char> body;
    RecvStatus rs = sent ? recv_msg(fd, out, body) : RecvStatus::CLOSED;
    bool well_defined = (rs == RecvStatus::CLOSED) ||
                        (rs == RecvStatus::OK && out.id == READBOARDINFO && out.error == 0);
    close(fd);

    // 另起连接确认服务端整体仍健康（与“单连接错位”解耦）。
    CHECK(ctx, fresh_server_ok(srv), "server no longer serves fresh connections after oversize frame");

    BUG_CHECK(ctx, well_defined,
              "S2: oversize frame desynced the connection (status=%d id=%d) —— 丢头不关连接，流错位",
              (int)rs, (int)out.id);
}

// ===========================================================================
// [XFAIL S1] bodysize 被截断为 u16：65536 → 0 长派发 → 同连接流错位
// ===========================================================================
TEST_XFAIL("protocol.bodysize_u16_truncation", TAG_PROTOCOL | TAG_DESTRUCTIVE, BUG_S1)
{
    PRIVATE_PROTO_SERVER(srv);

    int fd = raw_connect(srv.ip(), srv.port(), 1500);
    ASSERT(ctx, fd >= 0, "raw connect failed");

    // bodysize=65536：int 层面 > MAXMSGLEN（正确实现应拒绝），但服务端用 unsigned short 读 →
    //   e_pkgLen = (u16)65536 = 0 → 按“无包体”立即派发；而我们随后按 65536 发出的包体被当作后续
    //   包头流解析 → 同连接错位。
    const int BODY = 65536;  // 65536 % 180 = 16 → 确定性错位
    MSGHEAD h = make_head(READB, "BOARD", fx::TAG_BINMAX);
    h.datasize = BODY;
    h.bodysize = BODY;
    ASSERT(ctx, raw_send(fd, &h, sizeof(h)), "send truncating header failed");
    std::vector<char> blob(BODY, (char)0xFF);
    ASSERT(ctx, raw_send(fd, blob.data(), (int)blob.size()), "send truncated-intent body failed");

    CHECK(ctx, srv.running(), "server died on u16-truncated bodysize (unexpected)");

    // 排空服务端可能已因“0 长 READB”立即回出的响应，再探测流是否同步。
    // 正确行为：同连接合法往返得到 READBOARDINFO 响应，或连接被干净关闭；今日 → 流错位超时。
    MSGHEAD h2 = make_head(READBOARDINFO, "BOARD", "");
    h2.datasize = sizeof(BOARD_INFO);
    h2.bodysize = 0;
    bool sent = send_msg(fd, h2);
    bool got_boardinfo = false;
    RecvStatus rs = RecvStatus::CLOSED;
    if (sent) {
        // 最多读两帧：第一帧可能是被截断派发的 READB 残响，第二帧才是我们期望的 READBOARDINFO。
        for (int i = 0; i < 2; i++) {
            MSGHEAD out{};
            std::vector<char> body;
            rs = recv_msg(fd, out, body);
            if (rs != RecvStatus::OK) break;
            if (out.id == READBOARDINFO && out.error == 0) { got_boardinfo = true; break; }
        }
    }
    bool well_defined = got_boardinfo || rs == RecvStatus::CLOSED;
    close(fd);

    CHECK(ctx, fresh_server_ok(srv), "server no longer serves fresh connections after truncated frame");

    BUG_CHECK(ctx, well_defined,
              "S1: bodysize u16 truncation desynced the connection (status=%d) —— 65536→0 长派发",
              (int)rs);
}
