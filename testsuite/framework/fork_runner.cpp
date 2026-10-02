// fork_runner.cpp —— 见 fork_runner.h
#include "fork_runner.h"

#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <fcntl.h>
#include <cstdlib>

namespace ts {

bool ForkResult::crashed() const
{
    if (!signaled) return false;
    switch (signal) {
        case SIGSEGV:
        case SIGABRT:
        case SIGBUS:
        case SIGFPE:
        case SIGILL:
            return true;
        default:
            return false;
    }
}

const char* ForkResult::describe() const
{
    static char buf[96];
    if (timed_out)
        snprintf(buf, sizeof(buf), "timed out (killed)");
    else if (signaled)
        snprintf(buf, sizeof(buf), "killed by signal %d (%s)", signal, strsignal(signal));
    else if (exited)
        snprintf(buf, sizeof(buf), "exited with code %d", exit_code);
    else
        snprintf(buf, sizeof(buf), "unknown state");
    return buf;
}

ForkResult run_in_fork(const std::function<void()>& body, int timeout_ms)
{
    ForkResult r;
    char path[] = "/tmp/gplat_fork_XXXXXX";
    int log = mkstemp(path);
    if (log < 0) {
        perror("fork diagnostics mkstemp");
        return r;
    }
    unlink(path);
    fcntl(log, F_SETFD, FD_CLOEXEC);
    fflush(nullptr);  // 避免父子重复刷同一缓冲区

    pid_t pid = fork();
    if (pid < 0) {
        // fork 失败：当成未崩溃的异常返回，调用方会据此 CHECK 失败
        r.exited = true;
        r.exit_code = -1;
        perror("fork");
        close(log);
        return r;
    }
    if (pid == 0) {
        // 子进程：跑 body。任何异常都转成退出码，绝不把异常抛回框架。
        int code = 0;
        if (dup2(log, STDERR_FILENO) < 0) _exit(71);
        close(log);
        try {
            body();
        } catch (const std::exception& e) {
            fprintf(stderr, "fork body exception: %s\n", e.what());
            fflush(stderr);
            code = 70;
        } catch (...) {
            fprintf(stderr, "fork body threw an unknown exception\n");
            fflush(stderr);
            code = 70;  // EX_SOFTWARE：body 抛了异常
        }
        _exit(code);
    }

    // 父进程：带超时地等待子进程结束。
    const int stepUs = 2000;  // 2ms 轮询粒度
    int waitedUs = 0;
    int status = 0;
    while (true) {
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) break;              // 子进程已结束
        if (w < 0) {                      // 异常（EINTR 之外少见）
            if (errno == EINTR) continue;
            perror("fork waitpid");
            close(log);
            return r;
        }
        if (waitedUs >= timeout_ms * 1000) {
            // 超时：杀掉并回收，标记 timed_out
            kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            r.timed_out = true;
            r.signaled = true;
            r.signal = SIGKILL;
            break;
        }
        timespec ts{0, stepUs * 1000};
        nanosleep(&ts, nullptr);
        waitedUs += stepUs;
    }

    lseek(log, 0, SEEK_SET);
    char buf[4096];
    ssize_t n;
    while ((n = read(log, buf, sizeof(buf))) > 0 && r.diagnostics.size() < 1024 * 1024)
        r.diagnostics.append(buf, static_cast<size_t>(n));
    close(log);
    if (!r.timed_out && WIFEXITED(status)) {
        r.exited = true;
        r.exit_code = WEXITSTATUS(status);
    } else if (!r.timed_out && WIFSIGNALED(status)) {
        r.signaled = true;
        r.signal = WTERMSIG(status);
    }
    return r;
}

}  // namespace ts
