#include <cstdio>
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>
#include <unistd.h>
#include <signal.h>
#include <cstring>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <climits>
#include <cstdlib>

#include "s7config.h"
#include "s7log.h"

std::atomic<bool> g_running(true);
static int g_pidfile_fd = -1;   // PID文件描述符，用于文件锁
static std::string g_pidfile_path;

// 前向声明
void threadReadPlc(PlcConfig* plc, AppConfig* config);
void threadWritePlc(AppConfig* config);

// ---- 守护进程 ----
// 参考 gplat/ngx_daemon.cxx 风格: fork + setsid + umask + 重定向stdio到/dev/null
// 返回: 0=子进程(成功), 1=父进程(应退出), -1=失败
static int s7_daemon()
{
    switch (fork()) {
    case -1:
        fprintf(stderr, "s7_daemon(): fork() failed: %s\n", strerror(errno));
        return -1;
    case 0:
        // 子进程
        break;
    default:
        // 父进程，返回1让调用者退出
        return 1;
    }

    // 脱离终端，创建新会话
    if (setsid() == -1) {
        fprintf(stderr, "s7_daemon(): setsid() failed: %s\n", strerror(errno));
        return -1;
    }

    // 不限制文件权限
    umask(0);

    // 重定向stdin/stdout/stderr到/dev/null
    int fd = open("/dev/null", O_RDWR);
    if (fd == -1) {
        fprintf(stderr, "s7_daemon(): open(\"/dev/null\") failed: %s\n", strerror(errno));
        return -1;
    }
    if (dup2(fd, STDIN_FILENO) == -1) {
        fprintf(stderr, "s7_daemon(): dup2(STDIN) failed: %s\n", strerror(errno));
        return -1;
    }
    if (dup2(fd, STDOUT_FILENO) == -1) {
        fprintf(stderr, "s7_daemon(): dup2(STDOUT) failed: %s\n", strerror(errno));
        return -1;
    }
    if (dup2(fd, STDERR_FILENO) == -1) {
        return -1;
    }
    if (fd > STDERR_FILENO) {
        close(fd);
    }

    return 0; // 子进程
}

// ---- PID文件管理 ----

// 打开PID文件并加锁（daemon化之前调用，验证可写性和单实例）
// 返回: true=成功, false=失败
static bool lock_pidfile(const std::string& path)
{
    g_pidfile_path = path;

    g_pidfile_fd = open(path.c_str(), O_WRONLY | O_CREAT, 0644);
    if (g_pidfile_fd == -1) {
        fprintf(stderr, "Cannot open PID file %s: %s\n", path.c_str(), strerror(errno));
        return false;
    }

    // 非阻塞排他锁，如果已被锁定说明有另一个实例在运行
    if (flock(g_pidfile_fd, LOCK_EX | LOCK_NB) == -1) {
        fprintf(stderr, "Another instance is already running (PID file locked: %s)\n", path.c_str());
        close(g_pidfile_fd);
        g_pidfile_fd = -1;
        return false;
    }

    return true;
}

// 写入PID到已锁定的文件（daemon化之后调用，写子进程PID）
static bool write_pidfile()
{
    if (g_pidfile_fd == -1) return false;

    if (ftruncate(g_pidfile_fd, 0) == -1) {
        return false;
    }

    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d\n", getpid());
    if (write(g_pidfile_fd, buf, len) != len) {
        return false;
    }

    return true;
}

// 清理PID文件
static void remove_pidfile()
{
    if (g_pidfile_fd != -1) {
        flock(g_pidfile_fd, LOCK_UN);
        close(g_pidfile_fd);
        g_pidfile_fd = -1;
    }
    if (!g_pidfile_path.empty()) {
        unlink(g_pidfile_path.c_str());
        g_pidfile_path.clear();
    }
}

// ---- 用法提示 ----

static void print_usage(const char* progname)
{
    fprintf(stderr,
        "Usage: %s [-d] [-c configfile]\n"
        "  -d            Run as daemon (background)\n"
        "  -c configfile Config file path (default: s7ioserver.ini)\n"
        "  -h            Show this help\n",
        progname);
}

// ---- 信号处理 ----

void signalHandler(int sig) {
    g_running = false;
}

// ---- main ----

int main(int argc, char* argv[])
{
    // 解析命令行参数
    std::string configFile = "s7ioserver.ini";
    bool cmdline_daemon = false;
    int opt;

    while ((opt = getopt(argc, argv, "dc:h")) != -1) {
        switch (opt) {
        case 'd':
            cmdline_daemon = true;
            break;
        case 'c':
            configFile = optarg;
            break;
        case 'h':
        default:
            print_usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    // 加载配置（daemon化之前，错误可输出到终端）
    AppConfig config;
    if (!LoadConfig(configFile, config)) {
        fprintf(stderr, "Failed to load config file: %s\n", configFile.c_str());
        return 1;
    }

    // 命令行 -d 覆盖配置文件
    if (cmdline_daemon) {
        config.daemon_mode = true;
        fprintf(stdout, "以守护进程运行\n");
    }

    // 校验PLC配置（daemon化之前，错误可输出到终端）
    if (config.plcs.empty()) {
        fprintf(stderr, "No PLC configured. Exiting.\n");
        return 1;
    }

    // daemon化之前：将相对路径转为绝对路径（daemon后工作目录可能改变）
    if (config.daemon_mode) {
        auto to_abspath = [](std::string& path) {
            if (!path.empty() && path[0] != '/') {
                char cwd[PATH_MAX];
                if (getcwd(cwd, sizeof(cwd))) {
                    path = std::string(cwd) + "/" + path;
                }
            }
        };
        to_abspath(config.log_file);
        to_abspath(config.pid_file);
    }

    // daemon化之前：打开并锁定PID文件（验证可写性和单实例，错误可输出到终端）
    if (config.daemon_mode) {
        if (!lock_pidfile(config.pid_file)) {
            return 1;
        }
    }

    // 守护进程化（在日志初始化之前）
    if (config.daemon_mode) {
        int rc = s7_daemon();
        if (rc == -1) {
            fprintf(stderr, "Failed to daemonize. Exiting.\n");
            return 1;
        }

        if (rc == 1) {
            fprintf(stdout, "父进程，正常退出\n");
            // 父进程，正常退出（不清理PID文件，由子进程持有锁）
            // 关闭父进程的fd副本，子进程继承了锁
            if (g_pidfile_fd != -1) {
                close(g_pidfile_fd);
                g_pidfile_fd = -1;
            }
            return 0;
        }
        // 子进程继续，写入子进程PID
        write_pidfile();
    }

    // 注册信号处理（daemon化之后，确保子进程注册）
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    // 初始化日志（daemon化之后）
    s7log_init(config.log_file.c_str(), config.log_level,
               config.log_rotate_count, config.log_rotate_size);

    // 打印配置信息
    PrintConfig(config);

    s7log_info("s7ioserver started (PID %d). %zu PLC(s) configured.%s",
               getpid(), config.plcs.size(),
               config.daemon_mode ? " [daemon]" : "");

    // 每个PLC启动一个读线程
    std::vector<std::thread> readThreads;
    for (auto& plc : config.plcs) {
        readThreads.emplace_back(threadReadPlc, &plc, &config);
    }

	// 不要让读线程和写线程同时尝试连接PLC，先让读线程启动并连接成功，否则可能出现竞争条件导致连接失败。
    sleep(1);

    // 启动一个共享写线程
    std::thread writeThread(threadWritePlc, &config);

    // 主线程等待退出命令或信号
    while (true) {
        if (!g_running) break;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    g_running = false;

    // 等待所有线程结束
    for (auto& t : readThreads) {
        if (t.joinable()) t.join();
    }
    if (writeThread.joinable()) writeThread.join();

    s7log_info("s7ioserver exited.");
    s7log_close();

    // 清理PID文件
    if (config.daemon_mode) {
        remove_pidfile();
    }

    return 0;
}
