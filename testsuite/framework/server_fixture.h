// server_fixture.h —— gplat sandbox 启动器
//
// 职责（对应方案第 4 节）：
//   * 在 mkdtemp 临时目录建一棵与生产完全隔离的 sandbox：config/ qbdfile/ logs/ bin/(作 cwd)。
//   * 生成 gplat.conf（动态端口、Daemon=0 前台、可调线程数/连接数/回收秒数）。
//   * 启动前用“本地 API”在 forked 子进程里把标准 fixtures 建进 sandbox/qbdfile（不污染本进程全局态）。
//   * fork+exec 真实 gplat（按 /proc/self/exe 同目录定位，自动匹配普通/ASan 构建；rpath 自解析 ../lib）。
//   * 就绪探针（net_raw READBOARDINFO 往返，带超时，避免挂死）。
//   * 停机：SIGTERM 发给 master（worker 信号处理器为空，必须发 master）；超时则按进程组强杀。
//   * 解析 sandbox 下 asan.<pid> 报告，区分内存错误与泄漏。
//
// 关键隔离前提（务必遵守）：绝不触碰 8777 上的生产实例或仓库 qbdfile/。端口默认从 18777 起自动探测空闲端口。
#pragma once

#include <sys/types.h>

#include <functional>
#include <string>
#include <vector>

#include "higplat.h"  // connectgplat / disconnectgplat（供 ScopedConn）

namespace ts {

struct ServerConfig {
    int  port = 0;             // 0 = 从 preferredPort 起自动探测空闲端口
    int  preferredPort = 18777;
    int  threads = 3;          // ProcMsgRecvWorkThreadCount（发送路径/并发用例需 >=2）
    int  workerConns = 1024;   // worker_connections
    int  recyWait = 20;        // Sock_RecyConnectionWaitTime（churn 用例可调小/调大）
    int  logLevel = 8;
    bool asan = false;         // 期望运行于 ASan 构建：注入 ASAN_OPTIONS 做泄漏检测
    std::string suppFile;      // LSan 抑制文件路径（可空）
    bool populateFixtures = true;  // 启动前建标准 fixtures
    int  scale = 1;            // 传给 fixtures（放大容量等）
    // 自定义夹具构建器（可空）。非空时在 forked 子进程里以 qbdPath 调用它，取代标准 fixtures；
    // 返回失败项数（0=成功）。用于“私有小 BOARD”等特殊场景（容量溢出/并发竞态用例）。
    std::function<int(const char* qbdPath)> customFixtures;
    std::string gplatPath;     // 空 = 取 /proc/self/exe 同目录下的 gplat
    bool keepSandbox = false;  // true 或 env TS_KEEP_SANDBOX=1 时保留 sandbox 以便排查
    bool allowLeaks = false;   // 仅 shutdown-leak 特征化用例开启，不抑制内存错误
    bool allowAsanErrors = false; // 仅明确检查预期 ASan 故障的破坏性用例开启
};

// ASan/LSan 报告汇总。
struct AsanReport {
    bool found = false;      // 发现任何 asan.* 文件
    bool hasError = false;   // heap-buffer-overflow / use-after-free 等内存错误
    bool hasLeak = false;    // Direct/Indirect leak
    long leakBytes = 0;      // 泄漏字节合计
    int  leakBlocks = 0;     // 泄漏块数合计
    std::vector<std::string> files;  // 报告文件路径
    std::string summary;     // 摘要（首要错误行）
};

class ServerFixture {
public:
    explicit ServerFixture(const ServerConfig& cfg);
    ~ServerFixture();

    ServerFixture(const ServerFixture&) = delete;
    ServerFixture& operator=(const ServerFixture&) = delete;

    // 全流程启动：建 sandbox + fixtures + 起 server + 就绪探针。失败见 startup_error()。
    bool start();
    // 优雅停机（SIGTERM→master，等待退出；超时升级为强杀）。
    void stop();
    // 强杀整个进程组（SIGKILL），用于持久化崩溃用例。不等待优雅退出。
    void kill9();
    // 停机后原样复起（复用 sandbox/qbdfile，不重建 fixtures）——用于持久化/重启用例。
    bool relaunch();

    bool running();
    bool wait_stopped(int timeoutMs = 3000);
    const char* ip() const { return "127.0.0.1"; }
    int   port() const { return cfg_.port; }
    pid_t master_pid() const { return masterPid_; }
    // 最近一次 start()/relaunch() 中 gplat 若提前退出，其退出码（WEXITSTATUS）；
    // 被信号杀死或未发生提前退出时为 -1。用于启动失败的退出码断言（见持久化用例）。
    int   last_exit_code() const { return lastExitCode_; }
    pid_t worker_pid();  // 惰性扫描 /proc 找 ppid==master 的子进程；找不到返回 0

    const std::string& sandbox() const { return sandbox_; }
    const std::string& qbd_path() const { return qbdPath_; }
    const std::string& config_path() const { return confPath_; }

    // connectgplat 便捷封装（调用方负责 disconnectgplat）。失败返回 <0。
    int open_conn();

    // 解析 sandbox 下 asan.* 报告（通常在 stop() 之后调用）。
    AsanReport collect_asan();

    const std::string& startup_error() const { return startErr_; }

private:
    bool setup_sandbox();        // mkdtemp + 子目录 + 写 config + 可选 fixtures
    bool populate_fixtures();    // 在 forked 子进程里用本地 API 建 fixtures
    bool write_config();
    bool launch();               // fork/exec/就绪探针/找 worker
    bool wait_ready(int timeoutMs);
    void escalate_kill();        // kill(-pgid, SIGKILL) + 回收
    bool reap_master();
    bool reap_worker();

    ServerConfig cfg_;
    std::string sandbox_;        // sandbox 根目录
    std::string qbdPath_;        // <sandbox>/qbdfile
    std::string confPath_;       // <sandbox>/config/gplat.conf
    std::string logDir_;         // <sandbox>/logs
    std::string cwdDir_;         // <sandbox>/bin（gplat 的工作目录）
    std::string gplatBin_;       // 解析后的 gplat 可执行路径
    pid_t masterPid_ = -1;
    pid_t workerPid_ = 0;
    bool  alive_ = false;
    bool  forcedStop_ = false;
    int   lastExitCode_ = -1;    // gplat 提前退出时的 WEXITSTATUS（信号/未退出为 -1）
    std::string startErr_;
};

// 解析 /proc/self/exe 同目录，返回其中名为 name 的兄弟可执行路径。
std::string sibling_binary(const char* name);
// 从 preferred 起找一个可 bind 的空闲 TCP 端口（本机 127.0.0.1）。失败返回 0。
int pick_free_port(int preferred);

// RAII 连接句柄：构造即 connectgplat，析构即 disconnectgplat。
// 用例里用它持有 fd，可确保 ASSERT/异常展开时也会断开连接（避免 fd 泄漏污染 ASan）。
class ScopedConn {
public:
    explicit ScopedConn(ServerFixture& s) : fd_(s.open_conn()) {}
    explicit ScopedConn(int fd) : fd_(fd) {}
    ~ScopedConn() { if (fd_ >= 0) disconnectgplat(fd_); }
    ScopedConn(const ScopedConn&) = delete;
    ScopedConn& operator=(const ScopedConn&) = delete;
    ScopedConn(ScopedConn&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }

    int  fd() const { return fd_; }
    bool ok() const { return fd_ >= 0; }
    int  release() { int f = fd_; fd_ = -1; return f; }   // 放弃所有权（不再自动断开）
    void reset() { if (fd_ >= 0) { disconnectgplat(fd_); fd_ = -1; } }

private:
    int fd_;
};

}  // namespace ts
