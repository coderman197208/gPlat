// test_protocol.cpp —— 裸协议模糊 / 破坏性（方案 §7.5 / §7.12；登记表 S1/S2）
//
// 这些用例手工拼 MSGHEAD（见 net_raw.h），故意制造畸形帧，验证服务端健壮性：
//   [N] unknown_msgid_dropped  —— 未注册 MSGID → 服务端静默丢包；客户端应**超时**而非挂死，
//                                 且服务端仍存活、仍能为其它连接正常服务。
//   [N] partial_header_abort   —— 只发半个包头随即 RST → 服务端不得崩溃，仍能服务。
//   [S2] oversize_frame_desync —— bodysize 声明 > _PKG_MAX_LENGTH：服务端必须关闭该连接
//                                 （原缺陷 S2 已修复：以前“丢头但不关连接”，随后的包体被当作新包头
//                                 解析 → 同一连接流永久错位）。断言：连接被干净关闭（或仍同步），服务端存活。
//   [S1] bodysize_u16_truncation —— bodysize 在线协议是 int，服务端须用 int 读取
//                                 （原缺陷 S1 已修复：以前用 unsigned short，65536 截断为 0 → 0 长派发 → 流错位）。
//                                 发 bodysize=65536 → 超限 → 连接被关闭。
//   [N] read_size_out_of_range —— READB/READQ/READTYPE 的 datasize（应答缓冲区大小）为负或超过 MAXMSGLEN：
//                                 服务端以 ERROR_RECORDSIZE 拒绝，不得按该长度分配内存而崩溃。
//   [N] write_datasize_exceeds_body —— WRITEB/WRITEQ/WRITEBPLC 声明的 datasize 大于实际包体：
//                                 服务端以 ERROR_RECORDSIZE 拒绝，不得越界读包体，tag 内容不变。
//   [N] createqueue_bad_size   —— CREATEQUEUE 的 recsize/count<=0 或超大：服务端以 ERROR_PARAMETER_SIZE
//                                 拒绝，绝不建出 num==0 的队列（原缺陷：随后 writeq/peekq 的 %num → SIGFPE 杀死 worker）。
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
// [S2] bodysize > _PKG_MAX_LENGTH：必须关闭连接，不得留下错位的流
// ===========================================================================
TEST("protocol.oversize_frame_desync", TAG_PROTOCOL | TAG_DESTRUCTIVE)
{
    PRIVATE_PROTO_SERVER(srv);

    int fd = raw_connect(srv.ip(), srv.port(), 1500);
    ASSERT(ctx, fd >= 0, "raw connect failed");

    // 声明一个超限包体（16385 > _PKG_MAX_LENGTH=MAXMSGLEN=16384），并按协议把这些“包体”字节真的发出去。
    // 服务端在 ngx_wait_request_handler_proc_p1 命中 e_pkgLen>_PKG_MAX_LENGTH 应关闭连接；
    // 此后客户端继续发送包体可能因对端已关闭而失败（EPIPE/RST），属预期，不作断言。
    const int OVER = MAXMSGLEN + 1;  // 16385；16385 % 180 != 0 → 确定性错位
    MSGHEAD h = make_head(WRITEB, "BOARD", fx::TAG_BINMAX);
    h.datasize = OVER;
    h.bodysize = OVER;
    ASSERT(ctx, raw_send(fd, &h, sizeof(h)), "send oversize header failed");
    std::vector<char> blob(OVER, (char)0xFF);  // 0xFF 包体：被当包头时 bodysize=0xFFFF>MAX → 继续丢，稳定错位
    raw_send(fd, blob.data(), (int)blob.size());

    // 服务端存活（丢头不崩溃）——可靠断言。
    CHECK(ctx, srv.running(), "server died on oversize frame (unexpected: should just drop)");

    // 核心：连接被干净关闭（或仍同步），而非流错位超时。
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

    CHECK(ctx, well_defined,
              "S2 regression: oversize frame desynced the connection (status=%d id=%d)",
              (int)rs, (int)out.id);
}

// ===========================================================================
// [S1] bodysize 不得被截断为 u16：65536 应判超限并关闭连接
// ===========================================================================
TEST("protocol.bodysize_u16_truncation", TAG_PROTOCOL | TAG_DESTRUCTIVE)
{
    PRIVATE_PROTO_SERVER(srv);

    int fd = raw_connect(srv.ip(), srv.port(), 1500);
    ASSERT(ctx, fd >= 0, "raw connect failed");

    // bodysize=65536：int 层面 > MAXMSGLEN，应被拒绝并关闭连接（原先被截断为 0 → 0 长派发 → 错位）。
    const int BODY = 65536;  // 65536 % 180 = 16 → 确定性错位
    MSGHEAD h = make_head(READB, "BOARD", fx::TAG_BINMAX);
    h.datasize = BODY;
    h.bodysize = BODY;
    ASSERT(ctx, raw_send(fd, &h, sizeof(h)), "send truncating header failed");
    std::vector<char> blob(BODY, (char)0xFF);
    raw_send(fd, blob.data(), (int)blob.size());  // 服务端可能已关闭连接，发送失败属预期

    CHECK(ctx, srv.running(), "server died on u16-truncated bodysize (unexpected)");

    // 正确行为：连接被干净关闭，或同连接合法往返得到 READBOARDINFO 响应；不得流错位超时。
    MSGHEAD h2 = make_head(READBOARDINFO, "BOARD", "");
    h2.datasize = sizeof(BOARD_INFO);
    h2.bodysize = 0;
    bool sent = send_msg(fd, h2);
    bool got_boardinfo = false;
    RecvStatus rs = RecvStatus::CLOSED;
    if (sent) {
        // 最多读两帧，容忍可能先到的其它响应帧。
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

    CHECK(ctx, well_defined,
              "S1 regression: bodysize u16 truncation desynced the connection (status=%d)",
              (int)rs);
}

// 发一个只有包头的请求并收应答；返回应答的 error，连接异常时返回 ~0u。
static unsigned int head_only_roundtrip(int fd, const MSGHEAD& h, int expectId)
{
    if (!send_msg(fd, h)) return ~0u;
    MSGHEAD out{};
    std::vector<char> body;
    if (recv_msg(fd, out, body) != RecvStatus::OK || out.id != expectId || out.bodysize != 0 || !body.empty())
        return ~0u;
    return out.error;
}

// ===========================================================================
// [N] 读请求的 datasize 越界 → ERROR_RECORDSIZE，服务端存活
// ===========================================================================
TEST("protocol.read_size_out_of_range", TAG_PROTOCOL | TAG_DESTRUCTIVE)
{
    PRIVATE_PROTO_SERVER(srv);

    int fd = raw_connect(srv.ip(), srv.port(), 1500);
    ASSERT(ctx, fd >= 0, "raw connect failed");

    const int sizes[] = {-1, INT32_MIN, MAXMSGLEN + 1, INT32_MAX};
    for (int size : sizes) {
        MSGHEAD h = make_head(READB, "BOARD", fx::TAG_I32);
        h.datasize = size;
        CHECK(ctx, head_only_roundtrip(fd, h, READB) == ERROR_RECORDSIZE, "READB datasize=%d not rejected", size);

        h = make_head(READTYPE, "BOARD", fx::TAG_I32);
        h.datasize = size;
        CHECK(ctx, head_only_roundtrip(fd, h, READTYPE) == ERROR_RECORDSIZE, "READTYPE datasize=%d not rejected", size);

        h = make_head(READQ, "NO_SUCH_QUEUE", "");
        h.datasize = size;
        CHECK(ctx, head_only_roundtrip(fd, h, READQ) == ERROR_RECORDSIZE, "READQ datasize=%d not rejected", size);
    }

    // READBSTRING 的缓冲区大小超过 MAXMSGLEN 时按 MAXMSGLEN 处理，仍能正常读取
    MSGHEAD h = make_head(READBSTRING, "BOARD", fx::TAG_I32);
    h.datasize = INT32_MAX;
    CHECK(ctx, head_only_roundtrip(fd, h, READBSTRING) != ~0u, "READBSTRING with huge buffer size broke the connection");

    CHECK(ctx, valid_roundtrip(fd), "connection unusable after out-of-range read sizes");
    close(fd);
    CHECK(ctx, srv.running(), "server died on out-of-range read size");
}

// ===========================================================================
// [N] 写请求的 datasize 大于实际包体 → ERROR_RECORDSIZE，不越界读，tag 不变
// ===========================================================================
TEST("protocol.write_datasize_exceeds_body", TAG_PROTOCOL | TAG_DESTRUCTIVE)
{
    PRIVATE_PROTO_SERVER(srv);

    int fd = raw_connect(srv.ip(), srv.port(), 1500);
    ASSERT(ctx, fd >= 0, "raw connect failed");

    const int32_t before = 0x11223344;
    MSGHEAD h = make_head(WRITEB, "BOARD", fx::TAG_I32);
    h.datasize = sizeof(before);
    h.bodysize = sizeof(before);
    ASSERT(ctx, send_msg(fd, h, &before, sizeof(before)), "send WRITEB failed");
    MSGHEAD out{};
    std::vector<char> body;
    ASSERT(ctx, recv_msg(fd, out, body) == RecvStatus::OK && out.error == 0, "baseline WRITEB failed");

    // 声明 4 字节、实际只带 2 字节包体
    const int ids[] = {WRITEB, WRITEQ, WRITEBSTRING, WRITEBPLC, WRITEBSTRINGPLC};
    const char partial[2] = {0x55, 0x66};
    for (int id : ids) {
        h = make_head(id, id == WRITEQ ? "NO_SUCH_QUEUE" : "BOARD", fx::TAG_I32);
        h.datasize = sizeof(int32_t);
        h.bodysize = sizeof(partial);
        h.start = 1;
        ASSERT(ctx, send_msg(fd, h, partial, sizeof(partial)), "send short-body write failed");
        out = MSGHEAD{};
        ASSERT(ctx, recv_msg(fd, out, body) == RecvStatus::OK && out.id == id, "no reply for id=%d", id);
        CHECK(ctx, out.error == ERROR_RECORDSIZE, "id=%d short body: error=%u", id, out.error);
    }

    h = make_head(READB, "BOARD", fx::TAG_I32);
    h.datasize = sizeof(int32_t);
    ASSERT(ctx, send_msg(fd, h), "send READB failed");
    out = MSGHEAD{};
    ASSERT(ctx, recv_msg(fd, out, body) == RecvStatus::OK && out.error == 0 && body.size() == sizeof(int32_t), "READB failed");
    int32_t after = 0;
    memcpy(&after, body.data(), sizeof(after));
    CHECK(ctx, after == before, "tag changed by rejected writes: 0x%x", (unsigned)after);

    close(fd);
    CHECK(ctx, fresh_server_ok(srv), "server no longer serves fresh connections");
}

// ===========================================================================
// [N] CREATEQUEUE 的 recsize/count 非法 → ERROR_PARAMETER_SIZE，绝不建出会崩溃的队列
//     （原缺陷：count<=0 服务端照建 num==0 的队列，随后 writeq/peekq 的 %num → SIGFPE 杀死 worker；
//      客户端的 createqueue 校验只在客户端侧，裸协议可绕过。）
// ===========================================================================
TEST("protocol.createqueue_bad_size", TAG_PROTOCOL | TAG_DESTRUCTIVE)
{
    PRIVATE_PROTO_SERVER(srv);

    int fd = raw_connect(srv.ip(), srv.port(), 1500);
    ASSERT(ctx, fd >= 0, "raw connect failed");

    // {名字, recsize, count}：零/负/超大都必须被拒（超大 count 还会触发多 TB 文件分配）。
    struct { const char* name; int recsize; int count; } bad[] = {
        {"BADQ_C0",   64,  0},
        {"BADQ_CNEG", 64, -5},
        {"BADQ_CMAX", 64, INT32_MAX},
        {"BADQ_R0",    0,  4},
        {"BADQ_RNEG", -1,  4},
    };
    for (auto& b : bad) {
        MSGHEAD h = make_head(CREATEQUEUE, b.name, "");
        h.recsize = b.recsize;
        h.count = b.count;
        h.start = NORMAL_MODE;
        h.bodysize = 0;
        CHECK(ctx, head_only_roundtrip(fd, h, CREATEQUEUE) == ERROR_PARAMETER_SIZE,
              "CREATEQUEUE recsize=%d count=%d not rejected", b.recsize, b.count);

        // 被拒的队列不得存在：写它只会得到“不存在”，而不是让服务端崩溃。
        MSGHEAD w = make_head(WRITEQ, b.name, "");
        w.datasize = (b.recsize > 0 ? b.recsize : 64);
        w.bodysize = w.datasize;
        std::vector<char> payload(w.datasize, (char)0x5A);
        ASSERT(ctx, send_msg(fd, w, payload.data(), (int)payload.size()), "send WRITEQ failed");
        MSGHEAD out{};
        std::vector<char> body;
        ASSERT(ctx, recv_msg(fd, out, body) == RecvStatus::OK && out.id == WRITEQ,
               "no WRITEQ reply for %s", b.name);
        CHECK(ctx, out.error == ERROR_RECORD_NOT_EXIST,
              "WRITEQ on rejected queue %s: error=%u", b.name, out.error);
    }

    // 合法创建仍工作，且可正常读写（确认修复没有误伤正常路径）。
    MSGHEAD hc = make_head(CREATEQUEUE, "OKQ", "");
    hc.recsize = 64;
    hc.count = 8;
    hc.start = NORMAL_MODE;
    hc.bodysize = 0;
    CHECK(ctx, head_only_roundtrip(fd, hc, CREATEQUEUE) == 0, "valid CREATEQUEUE rejected");

    MSGHEAD hw = make_head(WRITEQ, "OKQ", "");
    hw.datasize = 64;
    hw.bodysize = 64;
    std::vector<char> rec(64, (char)0x5A);
    ASSERT(ctx, send_msg(fd, hw, rec.data(), (int)rec.size()), "send WRITEQ OKQ failed");
    MSGHEAD out{};
    std::vector<char> body;
    ASSERT(ctx, recv_msg(fd, out, body) == RecvStatus::OK && out.id == WRITEQ, "no WRITEQ OKQ reply");
    CHECK(ctx, out.error == 0, "valid queue write failed: error=%u", out.error);

    MSGHEAD hr = make_head(READQ, "OKQ", "");
    hr.datasize = 64;
    ASSERT(ctx, send_msg(fd, hr), "send READQ OKQ failed");
    out = MSGHEAD{};
    ASSERT(ctx, recv_msg(fd, out, body) == RecvStatus::OK && out.id == READQ, "no READQ OKQ reply");
    CHECK(ctx, out.error == 0 && body.size() == 64, "valid queue read failed: error=%u", out.error);

    close(fd);
    CHECK(ctx, srv.running(), "server died on malformed CREATEQUEUE");
    CHECK(ctx, fresh_server_ok(srv), "server no longer serves fresh connections after malformed CREATEQUEUE");
}
