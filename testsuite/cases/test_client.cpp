// test_client.cpp —— 客户端库的连接所有权与断线处理
//
// 约定：libhigplat 从不 close 调用方的 fd。流已不可信时（send/recv 失败、对端关闭、响应错位），库只
// shutdown(SHUT_RDWR) 并返回 CONNECTION 类错误码；此后该 fd 上的调用都以 CONNECTION 类错误快速失败，
// fd 号保持有效，直到调用方 disconnectgplat。其它错误不影响连接。
//   [N] peer_close_is_connection_error         —— 假服务端 accept 后立即关闭：readb 返回 CONNECTION 类错误
//                                                 （以前 readn 把 EOF 当成功）；再次调用不被 SIGPIPE 杀死；
//                                                 fd 直到 disconnectgplat 才关闭。
//   [N] invalid_response_shuts_down            —— 写请求的响应带包体：ERROR_INVALID_RESPONSE，fd 仍有效，
//                                                 对端收到 FIN（shutdown 而非 close）；后续调用返回 CONNECTION 类错误。
//   [N] buffer_too_small_keeps_connection      —— waitpostdata / listq 缓冲区不足：包体被读掉，返回
//                                                 ERROR_BUFFER_TOO_SMALL，连接与订阅保留，后续收发不错位。
//   [N] subscribe_missing_tag_keeps_connection —— 订阅不存在的 tag 只返回 ERROR_TAG_NOT_EXIST，连接与已有订阅保留。
//   [N] wrapper_owns_fd                        —— GplatConnection：CONNECTION 类错误时自己关闭 fd 再抛
//                                                 GplatConnectionError；Result 只返回码；缓冲区不足抛
//                                                 GplatUsageError，连接保留。
//
// 制造断线的用例在 fork 子进程里跑（SIGPIPE 恢复为默认处理，进程被杀即失败），并关闭错误钩子避免 stderr 噪音。
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "framework/ts.h"
#include "gplat_connection.h"

using namespace ts;

namespace {

constexpr const char* MISSING_TAG = "NO_SUCH_TAG_client";

// 在 127.0.0.1 的临时端口上监听，充当一次性的假服务端。失败返回 -1。
int listen_loopback(int& port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(addr);
    if (bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(fd, 4) != 0 ||
        getsockname(fd, (sockaddr*)&addr, &len) != 0) {
        close(fd);
        return -1;
    }
    port = ntohs(addr.sin_port);
    return fd;
}

// 接受一个连接，并给它设接收超时，避免假服务端读不到 FIN 时挂死。
int accept_with_timeout(int lfd, int timeoutMs)
{
    int fd = accept(lfd, nullptr, nullptr);
    if (fd >= 0) {
        timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    return fd;
}

bool fd_open(int fd) { return fcntl(fd, F_GETFD) != -1; }

bool is_connection_error(unsigned err) { return GetErrorCategory(err, nullptr) == GPLAT_ERRCAT_CONNECTION; }

int open_fd_count()
{
    int n = 0;
    DIR* d = opendir("/proc/self/fd");
    if (d == nullptr) return -1;
    while (readdir(d) != nullptr) n++;
    closedir(d);
    return n;
}

void quiet_child()
{
    signal(SIGPIPE, SIG_DFL);   // 不依赖调用方忽略 SIGPIPE
    SetErrorHook(nullptr, nullptr);
}

}  // namespace

// --- [N] 对端关闭：CONNECTION 类错误，不被 SIGPIPE 杀死，fd 归调用方 ----------------
TEST("client.peer_close_is_connection_error", TAG_CONN)
{
    ForkResult r = run_in_fork([]() {
        quiet_child();
        int port = 0;
        int lfd = listen_loopback(port);
        if (lfd < 0) _exit(77);
        int fd = connectgplat("127.0.0.1", port);
        int peer = fd < 0 ? -1 : accept(lfd, nullptr, nullptr);
        if (peer < 0) _exit(77);
        close(peer);   // 客户端先收到 FIN，随后的请求会被对端回 RST

        unsigned err = 0;
        int32_t v = 12345;
        if (readb(fd, fx::TAG_I32, &v, sizeof(v), &err, nullptr)) {
            fprintf(stderr, "readb after peer close returned true (err %u, value %d)\n", err, v);
            _exit(2);
        }
        if (!is_connection_error(err)) {
            fprintf(stderr, "readb after peer close: err %u is not a CONNECTION error\n", err);
            _exit(3);
        }
        if (!fd_open(fd)) {
            fprintf(stderr, "library closed the caller's fd\n");
            _exit(4);
        }

        // 连接已不可用：再次调用快速失败，send 不得触发 SIGPIPE
        err = 0;
        if (writeb(fd, fx::TAG_I32, &v, sizeof(v), &err) || !is_connection_error(err)) {
            fprintf(stderr, "second call: err %u\n", err);
            _exit(5);
        }
        if (!fd_open(fd)) _exit(6);
        disconnectgplat(fd);
        if (fd_open(fd)) _exit(7);
        close(lfd);
        _exit(0);
    }, 4000);

    ASSERT(ctx, !(r.exited && r.exit_code == 77), "fake server setup failed");
    CHECK(ctx, r.exited && r.exit_code == 0, "peer close (%s): %s", r.describe(), r.diagnostics.c_str());
}

// --- [N] 响应错位：shutdown 而非 close ------------------------------------------------
TEST("client.invalid_response_shuts_down", TAG_CONN)
{
    ForkResult r = run_in_fork([]() {
        quiet_child();
        int port = 0;
        int lfd = listen_loopback(port);
        if (lfd < 0) _exit(77);
        int fd = connectgplat("127.0.0.1", port);
        int peer = fd < 0 ? -1 : accept_with_timeout(lfd, 2000);
        if (peer < 0) _exit(77);

        // 先放好一个错位的响应：写请求的响应不应带包体
        MSGHEAD rsp = make_head(WRITEB);
        rsp.bodysize = 8;
        if (!raw_send(peer, &rsp, sizeof(rsp))) _exit(77);

        unsigned err = 0;
        int32_t v = 1;
        if (writeb(fd, fx::TAG_I32, &v, sizeof(v), &err) || err != ERROR_INVALID_RESPONSE) {
            fprintf(stderr, "writeb: err %u, expected ERROR_INVALID_RESPONSE\n", err);
            _exit(2);
        }
        if (!fd_open(fd)) {
            fprintf(stderr, "library closed the caller's fd\n");
            _exit(3);
        }

        // 对端收到完整请求，随后是 FIN
        MSGHEAD req{};
        int32_t body = 0;
        if (raw_recv(peer, &req, sizeof(req)) != RecvStatus::OK || req.id != WRITEB ||
            raw_recv(peer, &body, sizeof(body)) != RecvStatus::OK || body != v) {
            fprintf(stderr, "fake server did not receive the request\n");
            _exit(4);
        }
        char c = 0;
        RecvStatus st = raw_recv(peer, &c, 1);
        if (st != RecvStatus::CLOSED) {
            fprintf(stderr, "fake server: expected FIN after the invalid response, got status %d\n", (int)st);
            _exit(5);
        }

        err = 0;
        if (readb(fd, fx::TAG_I32, &v, sizeof(v), &err, nullptr) || !is_connection_error(err)) {
            fprintf(stderr, "call after shutdown: err %u\n", err);
            _exit(6);
        }
        disconnectgplat(fd);
        close(peer);
        close(lfd);
        _exit(0);
    }, 4000);

    ASSERT(ctx, !(r.exited && r.exit_code == 77), "fake server setup failed");
    CHECK(ctx, r.exited && r.exit_code == 0, "invalid response (%s): %s", r.describe(), r.diagnostics.c_str());
}

// --- [N] 缓冲区不足：读掉包体，连接与订阅保留 ------------------------------------------
TEST("client.buffer_too_small_keeps_connection", TAG_PUBSUB)
{
    const std::string ip = ctx.server().ip();
    const int port = ctx.server().port();

    ForkResult r = run_in_fork([&]() {
        quiet_child();
        int sub = connectgplat(ip.c_str(), port);
        int pub = connectgplat(ip.c_str(), port);
        unsigned err = 0;
        if (sub < 0 || pub < 0 || !subscribe(sub, fx::TAG_POST, &err)) _exit(77);

        std::vector<char> data(256);
        for (size_t i = 0; i < data.size(); i++) data[i] = (char)('a' + i % 26);
        if (!writeb(pub, fx::TAG_POST, data.data(), (int)data.size(), &err)) _exit(77);

        char name[GPLAT_TAGNAME_SIZE] = {0};
        char small[16];
        if (waitpostdata(sub, name, sizeof(name), small, sizeof(small), 2000, &err) || err != ERROR_BUFFER_TOO_SMALL) {
            fprintf(stderr, "waitpostdata small buffer: err %u\n", err);
            _exit(2);
        }
        if (!fd_open(sub)) {
            fprintf(stderr, "library closed the caller's fd\n");
            _exit(3);
        }

        // 流未错位：普通请求成功；订阅仍在，下一条事件完整
        int32_t v = 0;
        if (!readb(sub, fx::TAG_I32, &v, sizeof(v), &err, nullptr)) {
            fprintf(stderr, "readb after drained event: err %u\n", err);
            _exit(4);
        }
        data[0] = 'Z';
        if (!writeb(pub, fx::TAG_POST, data.data(), (int)data.size(), &err)) _exit(77);
        std::vector<char> big(1024, 0);
        if (!waitpostdata(sub, name, sizeof(name), big.data(), (int)big.size(), 2000, &err) || err != 0 ||
            strcmp(name, fx::TAG_POST) != 0 || memcmp(big.data(), data.data(), data.size()) != 0) {
            fprintf(stderr, "next event: err %u, tag '%s'\n", err, name);
            _exit(5);
        }

        // listq：名字列表放不进 1 字节缓冲区
        char tiny[1];
        int count = -1;
        if (listq(sub, tiny, sizeof(tiny), &count, &err) || err != ERROR_BUFFER_TOO_SMALL) {
            fprintf(stderr, "listq tiny buffer: err %u\n", err);
            _exit(6);
        }
        std::vector<char> names(GPLAT_MAX_DATA_SIZE);
        if (!listq(sub, names.data(), (int)names.size(), &count, &err) || count <= 0) {
            fprintf(stderr, "listq after drained list: err %u, count %d\n", err, count);
            _exit(7);
        }
        disconnectgplat(sub);
        disconnectgplat(pub);
        _exit(0);
    }, 6000);

    ASSERT(ctx, !(r.exited && r.exit_code == 77), "setup failed (%s): %s", r.describe(), r.diagnostics.c_str());
    CHECK(ctx, r.exited && r.exit_code == 0, "buffer too small (%s): %s", r.describe(), r.diagnostics.c_str());
}

// --- [N] 订阅不存在的 tag：只返回错误码，连接与已有订阅保留 ------------------------------
TEST("client.subscribe_missing_tag_keeps_connection", TAG_PUBSUB)
{
    ScopedConn sub(ctx.server());
    ScopedConn pub(ctx.server());
    ASSERT(ctx, sub.ok() && pub.ok(), "connect failed");
    unsigned err = 0;
    REQUIRE_OK(ctx, subscribe(sub.fd(), fx::TAG_POST, &err), err);

    EXPECT_ERR(ctx, subscribe(sub.fd(), MISSING_TAG, &err), err, ERROR_TAG_NOT_EXIST);
    ASSERT(ctx, fd_open(sub.fd()), "library closed the caller's fd");

    char data[256] = "still subscribed";
    REQUIRE_OK(ctx, writeb(pub.fd(), fx::TAG_POST, data, sizeof(data), &err), err);
    char name[GPLAT_TAGNAME_SIZE] = {0};
    char buf[256] = {0};
    EXPECT_OK(ctx, waitpostdata(sub.fd(), name, sizeof(name), buf, sizeof(buf), 2000, &err), err);
    CHECK(ctx, strcmp(name, fx::TAG_POST) == 0 && strcmp(buf, data) == 0, "event tag '%s' data '%s'", name, buf);

    EXPECT_OK(ctx, subscribe(sub.fd(), fx::TAG_I32, &err), err);
}

// --- [N] GplatConnection：CONNECTION 类错误自己关闭 fd，其余保留连接 -----------------------
TEST("client.wrapper_owns_fd", TAG_CONN)
{
    const std::string ip = ctx.server().ip();
    const int port = ctx.server().port();

    ForkResult r = run_in_fork([&]() {
        quiet_child();
        int fakePort = 0;
        int lfd = listen_loopback(fakePort);
        if (lfd < 0) _exit(77);

        const int before = open_fd_count();
        GplatConnection c("127.0.0.1", fakePort);
        if (!c.open() || open_fd_count() != before + 1) _exit(77);
        int peer = accept(lfd, nullptr, nullptr);
        if (peer < 0) _exit(77);
        close(peer);

        int32_t v = 0;
        try {
            (void)c.readb(fx::TAG_I32, &v, sizeof(v));
            fprintf(stderr, "readb after peer close did not throw\n");
            _exit(2);
        }
        catch (const GplatConnectionError& e) {
            if (!is_connection_error(e.code())) {
                fprintf(stderr, "GplatConnectionError code %u is not a CONNECTION error\n", e.code());
                _exit(3);
            }
        }
        if (c.is_open() || open_fd_count() != before) {
            fprintf(stderr, "wrapper did not close its fd: is_open %d, fds %d -> %d\n", c.is_open(), before,
                    open_fd_count());
            _exit(4);
        }
        close(lfd);

        GplatConnection sub(ip, port), pub(ip, port);
        if (!sub.open() || !pub.open()) _exit(77);
        if (sub.subscribe(MISSING_TAG) != ERROR_TAG_NOT_EXIST || !sub.is_open()) {
            fprintf(stderr, "subscribe missing tag must return ERROR_TAG_NOT_EXIST and keep the connection\n");
            _exit(5);
        }
        char data[256] = "wrapper";
        if (sub.subscribe(fx::TAG_POST) != 0 || pub.writeb(fx::TAG_POST, data, sizeof(data)) != 0) _exit(6);
        std::string name;
        char small[8];
        try {
            (void)sub.waitpostdata(name, small, sizeof(small), 2000);
            fprintf(stderr, "waitpostdata small buffer did not throw\n");
            _exit(7);
        }
        catch (const GplatUsageError& e) {
            if (e.code() != ERROR_BUFFER_TOO_SMALL) _exit(8);
        }
        if (!sub.is_open() || sub.readb(fx::TAG_I32, &v, sizeof(v)) != 0) {
            fprintf(stderr, "connection must stay usable after ERROR_BUFFER_TOO_SMALL\n");
            _exit(9);
        }
        _exit(0);
    }, 6000);

    ASSERT(ctx, !(r.exited && r.exit_code == 77), "setup failed (%s): %s", r.describe(), r.diagnostics.c_str());
    CHECK(ctx, r.exited && r.exit_code == 0, "wrapper (%s): %s", r.describe(), r.diagnostics.c_str());
}
