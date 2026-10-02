// fork_runner.h —— 子进程隔离执行器
//
// 用途：预期会“打崩客户端库”或触发进程级 abort/exit 的用例，放到 fork 出的子进程里跑，
//       父进程用 waitpid 判定子进程是正常退出、被信号打死（SIGSEGV/SIGABRT…）、还是超时挂死。
//       这样像 writeb_string(value=NULL) 的 strlen(NULL) 崩溃、FD_SET(fd>=1024) 栈溢出，
//       都不会带走整个测试进程；用例据 ForkResult 判定“是否如登记表所述崩溃”。
//
// 注意：被打崩的是客户端侧。服务端崩溃通过 ServerFixture 观察其退出，不走这里。
#pragma once

#include <functional>
#include <string>

namespace ts {

struct ForkResult {
    bool exited = false;     // 子进程 WIFEXITED（正常 return / _exit）
    int  exit_code = -1;    // exited 时的退出码
    bool signaled = false;   // 子进程被信号终止（WIFSIGNALED）
    int  signal = 0;        // 终止信号号
    bool timed_out = false;  // 超时被父进程 SIGKILL
    std::string diagnostics; // 捕获子进程 stderr（含 ASan 故障签名）
    bool sanitizer_error() const { return diagnostics.find("ERROR: AddressSanitizer") != std::string::npos; }

    // 典型“崩溃”形态：段错误 / abort / 总线错 / 浮点异常。
    bool crashed() const;
    // 人类可读的一行结论（用于断言信息）。
    const char* describe() const;
};

// 在子进程中运行 body；父进程至多等待 timeout_ms（默认 10s），超时则 SIGKILL 并回收。
// body 内抛出的 C++ 异常会被捕获并以退出码 70 结束子进程（EX_SOFTWARE）。
ForkResult run_in_fork(const std::function<void()>& body, int timeout_ms = 10000);

}  // namespace ts
