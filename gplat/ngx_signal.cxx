//和信号有关的函数放这里
//单进程多线程：不再用信号处理函数（处理函数里写日志要取 log_mutex，信号落在正持锁的线程上会自死锁），
//而是把需要处理的信号屏蔽掉，交给 signalfd，由 epoll 主线程像普通事件一样读取

#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <sys/signalfd.h>

#include "ngx_global.h"
#include "ngx_macro.h"
#include "ngx_func.h"

int g_signal_fd = -1;

//通过 signalfd 接收的信号
static void ngx_fill_signalfd_set(sigset_t* set)
{
	sigemptyset(set);
	sigaddset(set, SIGINT);      //终端中断符（Ctrl+C）
	sigaddset(set, SIGTERM);     //终止（kill / systemctl stop）
	sigaddset(set, SIGHUP);      //连接断开
	sigaddset(set, SIGQUIT);     //终端退出符
}

//描述：初始化信号，必须在创建任何线程之前调用（线程继承信号屏蔽字，这样信号只会经 signalfd 到达主线程）
//返回值：0成功，-1失败
int ngx_init_signals()
{
	//这些信号的默认动作会杀死进程（SIGPIPE：向已断开的管道/套接字写，如 stderr 接到已退出的管道），直接忽略；
	//写操作会返回 EPIPE（socket 发送用 MSG_NOSIGNAL）
	static const int ignored[] = { SIGPIPE, SIGSYS, SIGUSR1, SIGUSR2, SIGALRM, SIGIO, SIGWINCH };
	for (int signo : ignored)
	{
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = SIG_IGN;
		sigemptyset(&sa.sa_mask);
		if (sigaction(signo, &sa, NULL) == -1)
		{
			ngx_log_error_core(NGX_LOG_EMERG, errno, "sigaction(%d) failed", signo);
			return -1;
		}
	}

	sigset_t set;
	ngx_fill_signalfd_set(&set);
	if (pthread_sigmask(SIG_BLOCK, &set, NULL) != 0)
	{
		ngx_log_error_core(NGX_LOG_EMERG, 0, "pthread_sigmask() failed");
		return -1;
	}

	g_signal_fd = signalfd(-1, &set, SFD_NONBLOCK | SFD_CLOEXEC);
	if (g_signal_fd == -1)
	{
		ngx_log_error_core(NGX_LOG_EMERG, errno, "signalfd() failed");
		return -1;
	}
	return 0;
}

//描述：signalfd 可读时由 epoll 线程调用，读出全部待处理信号
//SIGINT / SIGTERM 置退出标志；SIGHUP / SIGQUIT 只记日志（没有 reload 功能，也不因终端断开而退出）
void ngx_handle_signals()
{
	for (;;)
	{
		struct signalfd_siginfo si;
		ssize_t n = read(g_signal_fd, &si, sizeof(si));
		if (n != (ssize_t)sizeof(si))
		{
			if (n == -1 && errno == EINTR)
				continue;
			break;      //EAGAIN：已读空
		}

		switch (si.ssi_signo)
		{
		case SIGINT:
		case SIGTERM:
			ngx_log_error_core(NGX_LOG_NOTICE, 0, "signal %d (%s) received from %P, shutting down",
				(int)si.ssi_signo, si.ssi_signo == SIGINT ? "SIGINT" : "SIGTERM", (pid_t)si.ssi_pid);
			g_stopEvent = 1;
			break;
		default:
			ngx_log_error_core(NGX_LOG_NOTICE, 0, "signal %d (%s) received from %P, ignored",
				(int)si.ssi_signo, si.ssi_signo == SIGHUP ? "SIGHUP" : "SIGQUIT", (pid_t)si.ssi_pid);
			break;
		}
	}
}

void ngx_close_signals()
{
	if (g_signal_fd != -1)
	{
		close(g_signal_fd);
		g_signal_fd = -1;
	}
}
