// server_fixture.cpp —— 见 server_fixture.h
#include "server_fixture.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>

#include "fixtures.h"
#include "fork_runner.h"
#include "higplat.h"   // connectgplat
#include "net_raw.h"
#include "test_registry.h"
#include "workload.h"
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace ts {

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------
std::string sibling_binary(const char* name)
{
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return name;
    buf[n] = '\0';
    std::string path(buf);
    size_t slash = path.find_last_of('/');
    std::string dir = (slash == std::string::npos) ? "." : path.substr(0, slash);
    return dir + "/" + name;
}

int pick_free_port(int preferred)
{
    for (int p = preferred; p < preferred + 500; p++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) continue;
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)p);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        bool ok = bind(fd, (sockaddr*)&addr, sizeof(addr)) == 0;
        close(fd);
        if (ok) return p;
    }
    return 0;
}

// 扫描 /proc 找第一个 ppid==parent 的进程。找不到返回 0。
static pid_t find_child_of(pid_t parent)
{
    DIR* d = opendir("/proc");
    if (!d) return 0;
    pid_t found = 0;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        // 只看纯数字目录
        const char* s = e->d_name;
        bool numeric = s[0] != '\0';
        for (const char* q = s; *q; q++) if (*q < '0' || *q > '9') { numeric = false; break; }
        if (!numeric) continue;

        char path[300];
        snprintf(path, sizeof(path), "/proc/%s/stat", s);
        FILE* f = fopen(path, "r");
        if (!f) continue;
        char line[512];
        if (fgets(line, sizeof(line), f)) {
            char* rp = strrchr(line, ')');  // comm 可能含空格/括号，取最后一个 ')'
            if (rp) {
                char state;
                int ppid = 0;
                if (sscanf(rp + 1, " %c %d", &state, &ppid) == 2 && ppid == (int)parent) {
                    found = (pid_t)atoi(s);
                }
            }
        }
        fclose(f);
        if (found) break;
    }
    closedir(d);
    return found;
}

// ---------------------------------------------------------------------------
// ServerFixture
// ---------------------------------------------------------------------------
ServerFixture::ServerFixture(const ServerConfig& cfg) : cfg_(cfg)
{
    if (cfg_.gplatPath.empty()) {
        if (auto* ctx = current_context(); ctx && ctx->runner)
            cfg_.gplatPath = ctx->runner->options().gplatPath;
    }
    if (!cfg_.keepSandbox) {
        const char* k = getenv("TS_KEEP_SANDBOX");
        if (k && k[0] == '1') cfg_.keepSandbox = true;
    }
}

ServerFixture::~ServerFixture()
{
    stop();
    if (auto* ctx = current_context()) {
        const int before = ctx->failures;
        if (forcedStop_) ctx->fail(__FILE__, __LINE__, "server graceful shutdown timed out");
        if (cfg_.asan) {
            AsanReport rep = collect_asan();
            if (rep.hasError && !cfg_.allowAsanErrors)
                ctx->fail(__FILE__, __LINE__, "ASan error: %s", rep.summary.c_str());
            if (rep.hasLeak && !cfg_.allowLeaks)
                ctx->fail(__FILE__, __LINE__, "LSan: %ld bytes / %d blocks", rep.leakBytes, rep.leakBlocks);
        }
        if (ctx->failures > before) cfg_.keepSandbox = true;
    }
    if (!cfg_.keepSandbox && !sandbox_.empty() &&
        sandbox_.find("gplat_sbx_") != std::string::npos) {
        std::error_code ec;
        fs::remove_all(sandbox_, ec);
        if (ec) fprintf(stderr, "[fixture] cleanup %s: %s\n", sandbox_.c_str(), ec.message().c_str());
    } else if (cfg_.keepSandbox && !sandbox_.empty()) {
        printf("    [sandbox kept] %s\n", sandbox_.c_str());
    }
}

bool ServerFixture::setup_sandbox()
{
    const char* tmp = getenv("TMPDIR");
    std::string base = (tmp && tmp[0]) ? tmp : "/tmp";
    std::string tmpl = base + "/gplat_sbx_XXXXXX";
    std::vector<char> t(tmpl.begin(), tmpl.end());
    t.push_back('\0');
    if (!mkdtemp(t.data())) {
        startErr_ = std::string("mkdtemp failed: ") + strerror(errno);
        return false;
    }
    sandbox_ = t.data();
    qbdPath_ = sandbox_ + "/qbdfile";
    confPath_ = sandbox_ + "/config/gplat.conf";
    logDir_ = sandbox_ + "/logs";
    cwdDir_ = sandbox_ + "/bin";

    std::error_code ec;
    for (const auto& dir : {sandbox_ + "/config", qbdPath_, logDir_, cwdDir_}) {
        fs::create_directories(dir, ec);
        if (ec) { startErr_ = "mkdir " + dir + ": " + ec.message(); return false; }
    }

    gplatBin_ = fs::absolute(cfg_.gplatPath.empty() ? sibling_binary("gplat") : cfg_.gplatPath).string();
    if (access(gplatBin_.c_str(), X_OK) != 0) {
        startErr_ = "gplat binary not executable: " + gplatBin_;
        return false;
    }

    if (cfg_.port == 0) {
        cfg_.port = pick_free_port(cfg_.preferredPort);
        if (cfg_.port == 0) {
            startErr_ = "no free port found";
            return false;
        }
        if (cfg_.port == 8777 || cfg_.port < 1024 || cfg_.port > 65535) {
            startErr_ = "unsafe test port (8777 is reserved for the existing instance)";
            return false;
        }
    }

    if (!write_config()) return false;

    if (cfg_.customFixtures || cfg_.populateFixtures) {
        if (!populate_fixtures()) return false;
    }
    return true;
}

bool ServerFixture::write_config()
{
    FILE* f = fopen(confPath_.c_str(), "w");
    if (!f) {
        startErr_ = std::string("cannot write config: ") + strerror(errno);
        return false;
    }
    // 注意：行首不可有空格（解析器把以空格开头的行当注释）；= 两侧空格会被 Rtrim/Ltrim 清掉。
    fprintf(f, "[Log]\n");
    fprintf(f, "Log=%s/gplat.log\n", logDir_.c_str());
    fprintf(f, "LogLevel = %d\n", cfg_.logLevel);
    fprintf(f, "LogRotateFileCount = 3\n");
    fprintf(f, "LogRotateFileSize = 5\n");
    fprintf(f, "[Proc]\n");
    fprintf(f, "WorkerProcesses = 1\n");
    fprintf(f, "Daemon = 0\n");  // 前台：exec 出来的进程即 master，便于拿 pid 与优雅退出
    fprintf(f, "ProcMsgRecvWorkThreadCount = %d\n", cfg_.threads);
    fprintf(f, "[Net]\n");
    fprintf(f, "ListenPortCount = 1\n");
    fprintf(f, "ListenPort0 = %d\n", cfg_.port);
    fprintf(f, "worker_connections = %d\n", cfg_.workerConns);
    fprintf(f, "Sock_RecyConnectionWaitTime = %d\n", cfg_.recyWait);
    fprintf(f, "[QBD]\n");
    fprintf(f, "QbdFilePath = %s\n", qbdPath_.c_str());
    bool ok = ferror(f) == 0;
    if (fclose(f) != 0) ok = false;
    if (!ok) startErr_ = "failed to flush test configuration";
    return ok;
}

bool ServerFixture::populate_fixtures()
{
    // 在 forked 子进程里用本地 API 建夹具，避免污染本进程的全局 dataQuePath / 已加载表。
    std::string qbd = qbdPath_;
    int scale = cfg_.scale;
    auto custom = cfg_.customFixtures;
    ForkResult r = run_in_fork(
        [qbd, scale, custom]() {
            int failures = custom ? custom(qbd.c_str()) : create_all_fixtures(qbd.c_str(), scale);
            _exit(failures == 0 ? 0 : 1);
        },
        30000);
    if (!(r.exited && r.exit_code == 0)) {
        startErr_ = std::string("fixture build failed: ") + r.describe();
        return false;
    }
    return true;
}

bool ServerFixture::start()
{
    if (!setup_sandbox()) return false;
    return launch();
}

bool ServerFixture::launch()
{
    fflush(nullptr);
    pid_t pid = fork();
    if (pid < 0) {
        startErr_ = std::string("fork failed: ") + strerror(errno);
        return false;
    }
    if (pid == 0) {
        // 子进程 → 变成 gplat master
        setpgid(0, 0);                 // 自成进程组，便于按组强杀
        if (chdir(cwdDir_.c_str()) != 0) _exit(126);

        // stdout/stderr 重定向到日志（定时器会每 500ms 打印，不影响）
        std::string outlog = logDir_ + "/stdout.log";
        int fd = open(outlog.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }

        // ASan/LSan 选项：日志写到 <sandbox>/asan.<pid>，不中止（便于 waitpid 优雅回收）
        std::string asanOpt = std::string(cfg_.asan ? "detect_leaks=1" : "detect_leaks=0") +
                              ":log_path=" + sandbox_ + "/asan" +
                              ":abort_on_error=0:halt_on_error=1:detect_stack_use_after_return=1";
        setenv("ASAN_OPTIONS", asanOpt.c_str(), 1);
        if (!cfg_.suppFile.empty()) {
            std::string lsanOpt = "suppressions=" + cfg_.suppFile + ":print_suppressions=0";
            setenv("LSAN_OPTIONS", lsanOpt.c_str(), 1);
        }

        execl(gplatBin_.c_str(), "gplat", (char*)nullptr);
        _exit(127);  // exec 失败
    }

    // 父进程
    masterPid_ = pid;
    workerPid_ = 0;
    alive_ = true;
    forcedStop_ = false;
    lastExitCode_ = -1;
    if (!wait_ready(15000)) {
        reap_master();
        startErr_ = masterPid_ <= 0 ? "gplat exited early with code " + std::to_string(lastExitCode_)
                                   : "gplat not ready within timeout";
        escalate_kill();
        alive_ = false;
        return false;
    }
    workerPid_ = find_child_of(masterPid_);
    if (workerPid_ <= 0) { startErr_ = "cannot identify fixture worker"; stop(); return false; }
    if (cfg_.asan) {
        std::ifstream maps("/proc/" + std::to_string(workerPid_) + "/maps");
        std::ostringstream text;
        text << maps.rdbuf();
        if (text.str().find("libasan.so") == std::string::npos) {
            startErr_ = "requested ASan but server has no libasan mapping";
            stop();
            return false;
        }
    }
    return true;
}

bool ServerFixture::wait_ready(int timeoutMs)
{
    int waited = 0;
    const int step = 100;
    while (waited < timeoutMs) {
        // master 是否已提前死亡
        int st = 0;
        if (waitpid(masterPid_, &st, WNOHANG) == masterPid_) {
            if (WIFEXITED(st)) lastExitCode_ = WEXITSTATUS(st);
            masterPid_ = -1;
            alive_ = false;
            return false;
        }
        int fd = raw_connect(ip(), cfg_.port, 500);
        if (fd >= 0) {
            MSGHEAD h = make_head(READBOARDINFO, "BOARD", "");
            h.datasize = sizeof(BOARD_INFO);
            h.bodysize = 0;
            MSGHEAD out{};
            std::vector<char> body;
            bool sent = send_msg(fd, h);
            RecvStatus rs = sent ? recv_msg(fd, out, body) : RecvStatus::PROTO_ERR;
            close(fd);
            if (sent && rs == RecvStatus::OK && out.id == READBOARDINFO &&
                out.error == 0 && body.size() == sizeof(BOARD_INFO) && !reap_master())
                return true;
        }
        timespec tss{0, step * 1000000};
        nanosleep(&tss, nullptr);
        waited += step;
    }
    return false;
}

void ServerFixture::stop()
{
    if (masterPid_ <= 0 && workerPid_ <= 0) { alive_ = false; return; }
    if (masterPid_ > 0) kill(masterPid_, SIGTERM);

    // 等待 master 优雅退出，最多 8 秒
    int waited = 0;
    const int step = 50;
    while (waited < 8000) {
        reap_master();
        reap_worker();
        if (masterPid_ <= 0 && workerPid_ <= 0) { alive_ = false; return; }
        timespec tss{0, step * 1000000};
        nanosleep(&tss, nullptr);
        waited += step;
    }
    // 超时 → 强杀整个进程组
    forcedStop_ = true;
    escalate_kill();
    alive_ = false;
}

void ServerFixture::kill9()
{
    escalate_kill();
    alive_ = false;
}

void ServerFixture::escalate_kill()
{
    if (workerPid_ <= 0 && masterPid_ > 0) workerPid_ = find_child_of(masterPid_);
    if (workerPid_ > 0) kill(workerPid_, SIGKILL);
    if (masterPid_ > 0) kill(masterPid_, SIGKILL);
    int st = 0;
    if (masterPid_ > 0)
        while (waitpid(masterPid_, &st, 0) < 0 && errno == EINTR) {}
    if (workerPid_ > 0)
        while (waitpid(workerPid_, &st, 0) < 0 && errno == EINTR) {}
    masterPid_ = -1;
    workerPid_ = 0;
    alive_ = false;
}

bool ServerFixture::reap_master()
{
    if (masterPid_ <= 0) return true;
    int st = 0;
    pid_t w = waitpid(masterPid_, &st, WNOHANG);
    if (w == masterPid_ || (w < 0 && errno == ECHILD)) {
        if (w > 0 && WIFEXITED(st)) lastExitCode_ = WEXITSTATUS(st);
        masterPid_ = -1;
        alive_ = false;
        return true;
    }
    return false;
}

bool ServerFixture::reap_worker()
{
    if (workerPid_ <= 0) return true;
    int st = 0;
    pid_t w = waitpid(workerPid_, &st, WNOHANG);
    if (w == workerPid_ || (w < 0 && errno == ECHILD && kill(workerPid_, 0) < 0 && errno == ESRCH)) {
        workerPid_ = 0;
        return true;
    }
    return false;
}

bool ServerFixture::running()
{
    reap_master();
    return masterPid_ > 0 && alive_;
}

bool ServerFixture::wait_stopped(int timeoutMs)
{
    const uint64_t deadline = now_nanos() + static_cast<uint64_t>(timeoutMs) * 1000000;
    do {
        reap_master();
        reap_worker();
        if (masterPid_ <= 0 && workerPid_ <= 0) return true;
        usleep(10000);
    } while (now_nanos() < deadline);
    return false;
}

bool ServerFixture::relaunch()
{
    // 复用现有 sandbox/qbdfile/config（不重建夹具），仅重新拉起进程 —— 持久化/重启用例用。
    return launch();
}

pid_t ServerFixture::worker_pid()
{
    if (workerPid_ > 0) {
        // 确认仍存活
        if (kill(workerPid_, 0) == 0) return workerPid_;
        workerPid_ = 0;
    }
    if (masterPid_ > 0) workerPid_ = find_child_of(masterPid_);
    return workerPid_;
}

int ServerFixture::open_conn()
{
    int fd = connectgplat(ip(), cfg_.port);
    if (fd >= 0) {
        timeval tv{5, 0};
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0 ||
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
            close(fd);
            return -1;
        }
    }
    return fd;
}

AsanReport ServerFixture::collect_asan()
{
    AsanReport rep;
    if (sandbox_.empty()) return rep;
    std::error_code ec;
    for (auto& ent : fs::directory_iterator(sandbox_, ec)) {
        std::string fn = ent.path().filename().string();
        if (fn.rfind("asan.", 0) != 0) continue;  // 只认 asan.<pid>
        rep.found = true;
        rep.files.push_back(ent.path().string());

        FILE* f = fopen(ent.path().c_str(), "r");
        if (!f) continue;
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            if (strstr(line, "ERROR: AddressSanitizer") ||
                strstr(line, "LeakSanitizer has encountered a fatal error") ||
                strstr(line, "AddressSanitizer:DEADLYSIGNAL")) {
                rep.hasError = true;
                if (rep.summary.empty()) {
                    rep.summary = line;
                    if (!rep.summary.empty() && rep.summary.back() == '\n') rep.summary.pop_back();
                }
            }
            if (strstr(line, "Direct leak") || strstr(line, "Indirect leak")) {
                rep.hasLeak = true;
                long bytes = 0, blocks = 0;
                // 形如: "Direct leak of 24 byte(s) in 1 object(s)"
                const char* of = strstr(line, "leak of ");
                if (of && sscanf(of + 8, "%ld byte(s) in %ld object(s)", &bytes, &blocks) == 2) {
                    rep.leakBytes += bytes;
                    rep.leakBlocks += (int)blocks;
                }
            }
            if (strstr(line, "SUMMARY: AddressSanitizer") && rep.summary.empty()) {
                rep.summary = line;
                if (!rep.summary.empty() && rep.summary.back() == '\n') rep.summary.pop_back();
            }
        }
        fclose(f);
    }
    if (ec) {
        rep.hasError = true;
        rep.summary = "cannot read ASan report directory: " + ec.message();
    }
    return rep;
}

}  // namespace ts
