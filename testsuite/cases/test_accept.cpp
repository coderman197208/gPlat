// test_accept.cpp —— 接入保护：连接数上限、fd 预算、fd 耗尽时不空转、TCP keepalive
//
//   [N] max_connections_enforced —— worker_connections 是客户端连接数上限（含监听 socket）：超出的连接被服务端
//                                   立即关闭（客户端马上读到 EOF，而不是连上后一直等不到应答），已建立的连接不受影响，
//                                   拒绝日志限频，断开一个后名额恢复。
//   [N] fd_limit_clamps_max_connections —— RLIMIT_NOFILE 不够支撑 worker_connections：服务端按 fd 上限降低连接上限并告警，
//                                   超出的连接同样被立即关闭，服务端自身不撞 EMFILE。
//   [N] emfile_rejects_pending —— 运行中 fd 被耗尽（启动后用 prlimit 压低 worker 的软上限，连接上限不再起保护作用）：
//                                   服务端借预留 fd 把排队连接取出立即关闭，客户端不会挂死；epoll 线程不空转；
//                                   EMFILE 日志限频；释放连接后恢复服务。
//                                   原缺陷：accept4 失败时连接留在已完成连接队列，监听 socket 水平触发持续可读，
//                                   epoll 线程空转（单核 100%）并每轮写一条 CRIT 日志（几十毫秒冲掉全部历史日志），
//                                   排队的客户端一直收不到应答。
//   [N] emfile_total_exhaustion_pauses —— 软上限压到预留 fd 的号上，腾出的名额也用不上：服务端暂停监听事件、
//                                   定期重试，epoll 线程不空转；恢复软上限后重新接受新连接。
//   [N] keepalive_configured —— 已接受的连接开启了 TCP keepalive 并使用 Sock_KeepAlive* 配置值；Sock_KeepAlive=0 时关闭。
//                                   （读 /proc/net/tcp 中服务端 socket 的定时器：已建立连接上 tr=2 即 keepalive 定时器）
//
// 隔离纪律：每个用例自建私有 ServerFixture（独立 sandbox + 自动空闲端口，绝不碰 8777）。
#include <arpa/inet.h>     // ntohs
#include <dirent.h>
#include <fcntl.h>         // O_CLOEXEC
#include <netinet/in.h>
#include <poll.h>
#include <sys/resource.h>  // prlimit
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "framework/ts.h"

using namespace ts;

namespace {

#define PRIVATE_ACCEPT_SERVER(SRV, CFG)                                          \
    ServerFixture SRV(CFG);                                                      \
    ASSERT(ctx, SRV.start(), "private accept fixture start failed: %s", SRV.startup_error().c_str())

ServerConfig base_config(TestContext& ctx)
{
    ServerConfig cfg;
    cfg.threads = 2;
    cfg.asan = ctx.expectAsan;
    return cfg;
}

const char* status_name(RecvStatus rs)
{
    switch (rs) {
    case RecvStatus::OK: return "OK";
    case RecvStatus::TIMEOUT: return "TIMEOUT";
    case RecvStatus::CLOSED: return "CLOSED";
    case RecvStatus::PROTO_ERR: return "PROTO_ERR";
    }
    return "?";
}

// 在已连接的裸 fd 上做一次 READBOARDINFO 往返。
// OK：收到正确应答；CLOSED：服务端关闭了连接（EOF/RST，含发送失败）；TIMEOUT：既没应答也没关闭（连接挂在 accept 队列里）
RecvStatus roundtrip(int fd)
{
    MSGHEAD h = make_head(READBOARDINFO, "BOARD", "");
    h.datasize = sizeof(BOARD_INFO);
    h.bodysize = 0;
    if (!send_msg(fd, h))
        return RecvStatus::CLOSED;
    MSGHEAD out{};
    std::vector<char> body;
    RecvStatus rs = recv_msg(fd, out, body);
    if (rs == RecvStatus::OK && !(out.id == READBOARDINFO && out.error == 0 && body.size() == sizeof(BOARD_INFO)))
        return RecvStatus::PROTO_ERR;
    return rs;
}

// 服务端在 timeoutMs 内关闭了这条（还没发过请求的）连接：读到 EOF 或 RST
bool closed_by_server(int fd, int timeoutMs)
{
    pollfd p{fd, POLLIN, 0};
    if (poll(&p, 1, timeoutMs) <= 0)
        return false;
    char c;
    ssize_t n = recv(fd, &c, 1, MSG_DONTWAIT);
    return n == 0 || (n < 0 && errno == ECONNRESET);
}

// 反复新建连接，直到一次往返成功（服务端处理断开、暂停到期恢复监听都需要一点时间），最多等 timeoutMs
bool served_within(ServerFixture& srv, int timeoutMs)
{
    const uint64_t deadline = now_nanos() + (uint64_t)timeoutMs * 1000000;
    do {
        int fd = raw_connect(srv.ip(), srv.port(), 1000);
        if (fd >= 0) {
            bool ok = roundtrip(fd) == RecvStatus::OK;
            close(fd);
            if (ok) return true;
        }
        usleep(50 * 1000);
    } while (now_nanos() < deadline);
    return false;
}

std::string read_text(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// gplat 日志里包含 needle 的行数
int count_log_lines(const ServerFixture& srv, const char* needle)
{
    std::istringstream in(read_text(srv.log_path()));
    int n = 0;
    for (std::string line; std::getline(in, line);)
        if (line.find(needle) != std::string::npos) n++;
    return n;
}

// worker 启动日志“客户端连接上限N（...”中的 N；找不到返回 -1
int logged_client_limit(const ServerFixture& srv)
{
    const std::string text = read_text(srv.log_path());
    const std::string marker = "客户端连接上限";
    for (size_t pos = text.find(marker); pos != std::string::npos; pos = text.find(marker, pos + 1)) {
        const char* p = text.c_str() + pos + marker.size();
        if (*p >= '0' && *p <= '9') return atoi(p);
    }
    return -1;
}

// worker 当前打开的 fd 号。reserve 非空时顺带找出预留 fd：指向 /dev/null 且带 O_CLOEXEC
// （从父进程继承来的 fd 不可能带 O_CLOEXEC，否则 exec 时已被关闭）
std::set<int> worker_fds(pid_t pid, int* reserve = nullptr)
{
    std::set<int> fds;
    const std::string proc = "/proc/" + std::to_string(pid);
    if (reserve) *reserve = -1;
    DIR* d = opendir((proc + "/fd").c_str());
    if (!d) return fds;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        const int fd = atoi(e->d_name);
        fds.insert(fd);
        if (!reserve) continue;
        char target[256] = {0};
        ssize_t n = readlink((proc + "/fd/" + e->d_name).c_str(), target, sizeof(target) - 1);
        if (n <= 0 || strcmp(target, "/dev/null") != 0) continue;
        std::istringstream info(read_text(proc + "/fdinfo/" + e->d_name));
        for (std::string line; std::getline(info, line);) {
            if (line.compare(0, 6, "flags:") == 0 && (strtoul(line.c_str() + 6, nullptr, 8) & O_CLOEXEC))
                *reserve = fd;
        }
    }
    closedir(d);
    return fds;
}

// 返回一个软上限，使号小于它的空闲 fd 恰好有 freeSlots 个（新 fd 总取最小的空闲号，号必须小于软上限）
int limit_with_free_slots(const std::set<int>& fds, int freeSlots)
{
    int n = 0;
    for (int freeCount = 0;; n++) {
        if (fds.count(n) == 0 && ++freeCount == freeSlots) return n + 1;
    }
}

// 只压低 worker 的 RLIMIT_NOFILE 软上限（硬上限不动，之后才能恢复）；saved 返回原值
bool set_worker_soft_nofile(pid_t pid, rlim_t soft, struct rlimit* saved)
{
    struct rlimit cur{};
    if (prlimit(pid, RLIMIT_NOFILE, nullptr, &cur) != 0) return false;
    if (saved) *saved = cur;
    struct rlimit want = cur;
    want.rlim_cur = soft;
    return prlimit(pid, RLIMIT_NOFILE, &want, nullptr) == 0;
}

// worker 主线程（epoll 线程，tid == pid）累计占用的 CPU 时钟滴答（utime + stime）
long main_thread_ticks(pid_t pid)
{
    const std::string text = read_text("/proc/" + std::to_string(pid) + "/task/" + std::to_string(pid) + "/stat");
    const size_t rp = text.rfind(')');
    if (rp == std::string::npos) return -1;
    std::istringstream in(text.substr(rp + 1));
    std::string field;
    long utime = -1, stime = -1;
    for (int i = 3; in >> field; i++) {  // ')' 之后是第 3 个字段（state）
        if (i == 14) utime = atol(field.c_str());
        if (i == 15) { stime = atol(field.c_str()); break; }
    }
    return (utime < 0 || stime < 0) ? -1 : utime + stime;
}

// ms 毫秒内 worker 主线程的 CPU 占用率（%），读不到返回 -1
double main_thread_cpu_pct(pid_t pid, int ms)
{
    const long t0 = main_thread_ticks(pid);
    usleep(ms * 1000);
    const long t1 = main_thread_ticks(pid);
    if (t0 < 0 || t1 < 0) return -1;
    return (double)(t1 - t0) * 100.0 / (double)sysconf(_SC_CLK_TCK) / (ms / 1000.0);
}

// 服务端一侧 socket（本地端口 = serverPort，对端端口 = clientFd 的本地端口）的 TCP 定时器，取自 /proc/net/tcp。
// tr：0 无定时器，1 重传，2 keepalive（已建立连接上 sk_timer 只用于 keepalive），4 零窗口探测；when：剩余时钟滴答
bool server_side_timer(int serverPort, int clientFd, unsigned* tr, unsigned long* when)
{
    sockaddr_in local{};
    socklen_t len = sizeof(local);
    if (getsockname(clientFd, (sockaddr*)&local, &len) != 0) return false;
    const unsigned clientPort = ntohs(local.sin_port);

    std::ifstream in("/proc/net/tcp");
    std::string line;
    std::getline(in, line);  // 表头
    while (std::getline(in, line)) {
        unsigned lip, lport, rip, rport, st, txq, rxq, t;
        unsigned long w;
        if (sscanf(line.c_str(), " %*u: %X:%X %X:%X %X %X:%X %X:%lX",
                   &lip, &lport, &rip, &rport, &st, &txq, &rxq, &t, &w) != 9)
            continue;
        if (lport == (unsigned)serverPort && rport == clientPort && st == 0x01) {  // 01 = ESTABLISHED
            *tr = t;
            *when = w;
            return true;
        }
    }
    return false;
}

}  // namespace

// ===========================================================================
// [N] worker_connections 是客户端连接数上限：超出的连接被立即关闭
// ===========================================================================
TEST("conn.max_connections_enforced", TAG_CONN)
{
    constexpr int kWorkerConns = 9;  // 含 1 个监听 socket → 最多 8 个客户端连接
    constexpr int kClients = kWorkerConns - 1;
    ServerConfig cfg = base_config(ctx);
    cfg.workerConns = kWorkerConns;
    PRIVATE_ACCEPT_SERVER(srv, cfg);
    CHECK(ctx, logged_client_limit(srv) == kClients, "logged client limit %d, expected %d",
          logged_client_limit(srv), kClients);
    usleep(100 * 1000);  // 就绪探针的连接已断开，等服务端处理完

    std::vector<int> held;
    for (int i = 0; i < kClients; i++) {
        int fd = raw_connect(srv.ip(), srv.port(), 2000);
        ASSERT(ctx, fd >= 0, "connect client %d failed", i);
        held.push_back(fd);
        RecvStatus rs = roundtrip(fd);
        ASSERT(ctx, rs == RecvStatus::OK, "client %d within the limit not served (%s)", i, status_name(rs));
    }

    // 超出上限：TCP 握手由内核完成，connect 照样成功，但服务端立即关闭连接，客户端马上读到 EOF
    constexpr int kOver = 100;
    int closed = 0;
    const uint64_t t0 = now_nanos();
    for (int i = 0; i < kOver; i++) {
        int fd = raw_connect(srv.ip(), srv.port(), 2000);
        ASSERT(ctx, fd >= 0, "connect over-limit client %d failed", i);
        if (closed_by_server(fd, 2000)) closed++;
        close(fd);
    }
    printf("    [info] %d/%d over-limit connections closed by the server in %.0f ms\n", closed, kOver,
           (double)(now_nanos() - t0) / 1e6);
    CHECK(ctx, closed == kOver, "%d over-limit connections were left open (the client would wait forever)", kOver - closed);

    for (int i = 0; i < kClients; i++)
        CHECK(ctx, roundtrip(held[i]) == RecvStatus::OK, "established client %d affected by the rejections", i);

    // 一阵拒绝只记一条日志（限频 5 秒）
    const int lines = count_log_lines(srv, "客户端连接数已达上限");
    CHECK(ctx, lines >= 1 && lines <= 2, "over-limit log lines = %d, expected 1..2 (rate-limited)", lines);

    // 断开一个，名额恢复
    close(held.back());
    held.pop_back();
    CHECK(ctx, served_within(srv, 2000), "the slot freed by a disconnect was not reusable");

    for (int fd : held) close(fd);
    CHECK(ctx, srv.running(), "server died");
}

// ===========================================================================
// [N] RLIMIT_NOFILE 不够支撑 worker_connections：按 fd 上限降低连接上限，服务端不撞 EMFILE
// ===========================================================================
TEST("conn.fd_limit_clamps_max_connections", TAG_CONN)
{
    constexpr int kNofile = 64;
    ServerConfig cfg = base_config(ctx);
    cfg.nofile = kNofile;  // 软/硬上限都是 64，服务端抬不高；worker_connections 仍是 1024
    PRIVATE_ACCEPT_SERVER(srv, cfg);

    const int limit = logged_client_limit(srv);
    printf("    [info] client limit under RLIMIT_NOFILE=%d: %d\n", kNofile, limit);
    ASSERT(ctx, limit >= 1 && limit < kNofile - 16, "unexpected client limit %d", limit);
    CHECK(ctx, count_log_lines(srv, "不足以支持worker_connections") == 1, "missing the clamp warning");
    usleep(100 * 1000);

    std::vector<int> held;
    for (int i = 0; i < limit; i++) {
        int fd = raw_connect(srv.ip(), srv.port(), 2000);
        ASSERT(ctx, fd >= 0, "connect client %d failed", i);
        held.push_back(fd);
        RecvStatus rs = roundtrip(fd);
        ASSERT(ctx, rs == RecvStatus::OK, "client %d/%d within the clamped limit not served (%s)", i, limit,
               status_name(rs));
    }
    int extra = raw_connect(srv.ip(), srv.port(), 2000);
    ASSERT(ctx, extra >= 0, "connect over-limit client failed");
    CHECK(ctx, closed_by_server(extra, 2000), "the connection beyond the clamped limit was not closed promptly");
    close(extra);

    CHECK(ctx, count_log_lines(srv, "Too many open files") == 0, "server ran into EMFILE despite the clamped limit");
    for (int fd : held) close(fd);
    CHECK(ctx, srv.running(), "server died");
}

// ===========================================================================
// [N] 运行中 fd 被耗尽：排队连接被立即关闭，epoll 线程不空转
// ===========================================================================
TEST("conn.emfile_rejects_pending", TAG_CONN)
{
    constexpr int kFree = 6;
    ServerConfig cfg = base_config(ctx);
    PRIVATE_ACCEPT_SERVER(srv, cfg);
    const pid_t wpid = srv.worker_pid();
    ASSERT(ctx, wpid > 0, "cannot resolve worker pid");
    usleep(100 * 1000);

    // 启动时按 worker_connections 算出的连接上限此后不再起保护作用：只留 kFree 个可用 fd 号，第 kFree+1 个连接撞 EMFILE
    const std::set<int> fds = worker_fds(wpid);
    ASSERT(ctx, !fds.empty(), "cannot list worker fds");
    const int soft = limit_with_free_slots(fds, kFree);
    if (!set_worker_soft_nofile(wpid, (rlim_t)soft, nullptr)) {
        ctx.skip(std::string("prlimit on the worker failed: ") + strerror(errno));
        return;
    }

    std::vector<int> held;
    int rejected = 0, hung = 0, other = 0;
    for (int i = 0; i < 16 && rejected < 6; i++) {
        int fd = raw_connect(srv.ip(), srv.port(), 2000);
        ASSERT(ctx, fd >= 0, "connect %d failed", i);
        RecvStatus rs = roundtrip(fd);
        if (rs == RecvStatus::OK) {
            held.push_back(fd);
            continue;
        }
        if (rs == RecvStatus::CLOSED) rejected++;
        else if (rs == RecvStatus::TIMEOUT) hung++;
        else other++;
        close(fd);
    }
    printf("    [info] soft limit %d (%d free): %zu served, %d rejected, %d hung, %d other\n", soft, kFree,
           held.size(), rejected, hung, other);
    CHECK(ctx, hung == 0, "%d connections got neither a reply nor a close (stuck in the accept queue)", hung);
    CHECK(ctx, other == 0, "%d connections got a malformed reply", other);
    CHECK(ctx, rejected >= 6, "only %d connections were rejected", rejected);
    CHECK(ctx, (int)held.size() == kFree, "%zu connections served, expected %d", held.size(), kFree);

    // fd 耗尽期间 epoll 线程不得空转
    const double cpu = main_thread_cpu_pct(wpid, 1000);
    printf("    [info] worker epoll thread CPU while fds are exhausted: %.1f%%\n", cpu);
    CHECK(ctx, cpu >= 0 && cpu < 20.0, "worker epoll thread used %.1f%% CPU while fds were exhausted (busy loop?)", cpu);

    // EMFILE 日志限频
    const int crit = count_log_lines(srv, "Too many open files");
    CHECK(ctx, crit >= 1 && crit <= 2, "EMFILE log lines = %d, expected 1..2 (rate-limited)", crit);

    for (size_t i = 0; i < held.size(); i++)
        CHECK(ctx, roundtrip(held[i]) == RecvStatus::OK, "established client %zu affected", i);

    // 释放两个连接后恢复接受新连接
    for (int k = 0; k < 2; k++) {
        close(held.back());
        held.pop_back();
    }
    CHECK(ctx, served_within(srv, 2000), "server did not accept new connections after fds were freed");

    for (int fd : held) close(fd);
    CHECK(ctx, srv.running(), "server died");
}

// ===========================================================================
// [N] 连预留 fd 腾出的名额也用不上：暂停监听事件、定期重试，不空转；恢复后重新接受连接
// ===========================================================================
TEST("conn.emfile_total_exhaustion_pauses", TAG_CONN)
{
    ServerConfig cfg = base_config(ctx);
    PRIVATE_ACCEPT_SERVER(srv, cfg);
    const pid_t wpid = srv.worker_pid();
    ASSERT(ctx, wpid > 0, "cannot resolve worker pid");
    usleep(100 * 1000);

    // 预留 fd 打开时取的是最小空闲号，比它小的号都在用；软上限压到这个号上：任何 accept 都 EMFILE，
    // 关掉预留 fd 腾出的号不小于软上限，同样用不上 → 只能暂停监听事件
    int reserve = -1;
    worker_fds(wpid, &reserve);
    ASSERT(ctx, reserve > 2, "cannot find the worker's reserved fd (/dev/null, O_CLOEXEC)");
    struct rlimit saved{};
    if (!set_worker_soft_nofile(wpid, (rlim_t)reserve, &saved)) {
        ctx.skip(std::string("prlimit on the worker failed: ") + strerror(errno));
        return;
    }

    // 这条连接留在 accept 队列里：既无法被 accept，也不能让 epoll 线程空转
    int pending = raw_connect(srv.ip(), srv.port(), 1000);
    ASSERT(ctx, pending >= 0, "connect failed");
    RecvStatus rs = roundtrip(pending);
    printf("    [info] request while no fd is available: %s\n", status_name(rs));
    CHECK(ctx, rs == RecvStatus::TIMEOUT, "expected the request to stay unanswered while fds are exhausted (%s)",
          status_name(rs));

    const double cpu = main_thread_cpu_pct(wpid, 1500);
    printf("    [info] worker epoll thread CPU while accept is paused: %.1f%%\n", cpu);
    CHECK(ctx, cpu >= 0 && cpu < 20.0, "worker epoll thread used %.1f%% CPU while fds were exhausted (busy loop?)", cpu);

    const int crit = count_log_lines(srv, "Too many open files");
    CHECK(ctx, crit >= 1 && crit <= 2, "EMFILE log lines = %d, expected 1..2 (rate-limited)", crit);
    CHECK(ctx, count_log_lines(srv, "暂停接受新连接") >= 1, "accept was not paused");

    // 恢复软上限：暂停到期后重新接受连接（排队的那条也会被处理）
    ASSERT(ctx, prlimit(wpid, RLIMIT_NOFILE, &saved, nullptr) == 0, "restoring the worker's fd limit failed");
    CHECK(ctx, served_within(srv, 2000), "server did not resume accepting after the fd limit was restored");

    close(pending);
    CHECK(ctx, srv.running(), "server died");
}

// ===========================================================================
// [N] 已接受的连接开启 TCP keepalive 并使用配置值；Sock_KeepAlive=0 时关闭
// ===========================================================================
TEST("conn.keepalive_configured", TAG_CONN)
{
    const long hz = sysconf(_SC_CLK_TCK);
    constexpr int kIdle = 40;  // 不同于缺省 60 秒，用来确认读的是配置值

    struct Case {
        std::vector<std::string> conf;
        bool enabled;
    };
    const Case cases[] = {
        {{"Sock_KeepAliveIdle = 40", "Sock_KeepAliveInterval = 5", "Sock_KeepAliveCount = 3"}, true},
        {{"Sock_KeepAlive = 0"}, false},
    };

    for (const Case& c : cases) {
        ServerConfig cfg = base_config(ctx);
        cfg.extraConf = c.conf;
        PRIVATE_ACCEPT_SERVER(srv, cfg);

        int fd = raw_connect(srv.ip(), srv.port(), 2000);
        ASSERT(ctx, fd >= 0, "connect failed");
        ASSERT(ctx, roundtrip(fd) == RecvStatus::OK, "roundtrip failed");
        usleep(200 * 1000);  // 等应答的 ACK 回来，重传定时器撤销

        unsigned tr = 0;
        unsigned long when = 0;
        const bool found = server_side_timer(srv.port(), fd, &tr, &when);
        close(fd);
        ASSERT(ctx, found, "server-side socket not found in /proc/net/tcp");
        printf("    [info] keepalive %s: tr=%u when=%lu ticks\n", c.enabled ? "on" : "off", tr, when);
        if (c.enabled) {
            CHECK(ctx, tr == 2, "keepalive timer not armed on the accepted socket (tr=%u)", tr);
            CHECK(ctx, when > (unsigned long)((kIdle - 10) * hz) && when <= (unsigned long)(kIdle * hz),
                  "keepalive timer %lu ticks, expected about %d s (Sock_KeepAliveIdle)", when, kIdle);
        } else {
            CHECK(ctx, tr == 0, "a timer is armed although Sock_KeepAlive = 0 (tr=%u)", tr);
        }
        CHECK(ctx, srv.running(), "server died");
    }
}
