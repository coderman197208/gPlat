
//和网络中接受连接【accept】有关的函数

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>    //uintptr_t
#include <stdarg.h>    //va_start....
#include <unistd.h>    //STDERR_FILENO等
#include <sys/time.h>  //gettimeofday
#include <time.h>      //localtime_r
#include <fcntl.h>     //open
#include <errno.h>     //errno
#include <sys/ioctl.h> //ioctl
#include <arpa/inet.h>
#include <dirent.h>        //opendir
#include <sys/resource.h>  //getrlimit/setrlimit
#include <netinet/in.h>
#include <netinet/tcp.h>   //TCP_KEEPIDLE/TCP_KEEPINTVL/TCP_KEEPCNT

#include "ngx_c_conf.h"
#include "ngx_macro.h"
#include "ngx_global.h"
#include "ngx_func.h"
#include "ngx_c_socket.h"

namespace
{
	//暂停/限频计时用单调时钟，不受系统校时影响
	int64_t MonotonicMs()
	{
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
	}

	//当前进程已打开的 fd 数；读不到 /proc 时返回一个保守值
	int CountOpenFds()
	{
		DIR* dir = opendir("/proc/self/fd");
		if (dir == NULL)
			return 64;
		int n = 0;
		while (readdir(dir) != NULL)
			++n;
		closedir(dir);
		return n - 3; //不算 "."、".." 和 opendir 自己占用的 fd
	}
}

//建立新连接专用函数，当新连接进入时，本函数会被ngx_epoll_process_events()所调用
void CSocekt::ngx_event_accept(lpngx_connection_t oldc)
{
	//因为listen套接字上用的不是ET【边缘触发】，而是LT【水平触发】，意味着客户端连入如果我要不处理，这个函数会被多次调用，所以这里可以不必多次accept()，可以只执行一次accept()
	//这也可以避免本函数被卡太久，注意，本函数应该尽快返回，以免阻塞程序运行；
	struct sockaddr    mysockaddr;        //远端服务器的socket地址
	socklen_t          socklen;
	int                err;
	int                level;
	int                s;
	static int         use_accept4 = 1;   //我们先认为能够使用accept4()函数
	lpngx_connection_t newc;              //代表连接池中的一个连接【注意这是指针】

	socklen = sizeof(mysockaddr);
	do
	{
		if (use_accept4)
		{
			//以为listen套接字是非阻塞的，所以即便已完成连接队列为空，accept4()也不会卡在这里；
			s = accept4(oldc->fd, &mysockaddr, &socklen, SOCK_NONBLOCK); //从内核获取一个用户端连接，最后一个参数SOCK_NONBLOCK表示返回一个非阻塞的socket，节省一次ioctl【设置为非阻塞】调用
		}
		else
		{
			//以为listen套接字是非阻塞的，所以即便已完成连接队列为空，accept()也不会卡在这里；
			s = accept(oldc->fd, &mysockaddr, &socklen);
		}

		//惊群，有时候不一定完全惊动所有4个worker进程，可能只惊动其中2个等等，其中一个成功其余的accept4()都会返回-1；错误 (11: Resource temporarily unavailable【资源暂时不可用】) 
		//所以参考资料：https://blog.csdn.net/russell_tao/article/details/7204260
		//其实，在linux2.6内核上，accept系统调用已经不存在惊群了（至少我在2.6.18内核版本上已经不存在）。大家可以写个简单的程序试下，在父进程中bind,listen，然后fork出子进程，
		//所有的子进程都accept这个监听句柄。这样，当新连接过来时，大家会发现，仅有一个子进程返回新建的连接，其他子进程继续休眠在accept调用上，没有被唤醒。
		if (s == -1)
		{
			err = errno;

			//对accept、send和recv而言，事件未发生时errno通常被设置成EAGAIN（意为“再来一次”）
			if (err == EAGAIN) //accept()没准备好
			{
				//除非你用一个循环不断的accept()取走所有的连接，不然一般不会有这个错误【我们这里只取一个连接，也就是accept()一次】
				return;
			}

			//EMFILE:进程的fd已用尽；ENFILE:系统的fd已用尽；ENOBUFS/ENOMEM:内存不足
			//这几种情况下连接不会从内核的已完成连接队列中取出，监听socket一直可读，水平触发下epoll_wait会立刻再次返回，
			//不处理就会空转并每轮写一条日志，由ngx_accept_exhausted()拒绝排队的连接或暂停监听事件
			if (err == EMFILE || err == ENFILE || err == ENOBUFS || err == ENOMEM)
			{
				ngx_accept_exhausted(oldc, err);
				return;
			}

			level = NGX_LOG_ALERT;
			if (err == ECONNABORTED)
			{
				//ECONNRESET错误则发生在对方意外关闭套接字后【您的主机中的软件放弃了一个已建立的连接--由于超时或者其它失败而中止接连(用户插拔网线就可能有这个错误出现)】
				//该错误被描述为“software caused connection abort”，即“软件引起的连接中止”，原因在于当服务和客户进程在完成用于TCP连接的三次握手后，
				//客户TCP却发送了一个RST（复位）分节，在服务进程看来，就在该连接已由TCP排队，等着服务进程调用accept的时候RST却到达了。
				//POSIX规定此时的errno值必须为ECONNABORTED。源自 Berkeley 的实现完全在内核中处理中止的连接，服务进程将永远不知道该中止的发生。
				//服务器进程一般可以忽略该错误，直接再次调用accept。
				level = NGX_LOG_ERR;
			}
			ngx_log_error_core(level, errno, "CSocekt::ngx_event_accept()中accept4()失败!");

			if (use_accept4 && err == ENOSYS) //accept4()函数没实现
			{
				use_accept4 = 0;  //标记不使用accept4()函数，改用accept()函数
				continue;         //回去重新用accept()函数搞
			}
			return;
		}

		//走到这里表示accept4()/accept()成功了

		//客户端连接数已达上限：立即关闭。客户端马上读到EOF，按连接错误处理（重连），而不是连上之后一直等不到应答；
		//上限低于fd上限，正常过载时客户端连接用不光fd
		if (m_onlineUserCount >= m_maxClientConnections)
		{
			close(s);
			unsigned long suppressed;
			if (m_logOverLimit.Allow(MonotonicMs() / 1000, &suppressed))
			{
				ngx_log_error_core(NGX_LOG_WARN, 0, "客户端连接数已达上限%d，拒绝新连接（上次记录后另拒绝%L个）",
					m_maxClientConnections, (int64_t)suppressed);
			}
			return;
		}

		//预留fd在fd耗尽时被用掉、又没能重新打开：现在有空余fd，补上
		if (m_idleFd == -1)
			m_idleFd = open("/dev/null", O_RDONLY | O_CLOEXEC);

		newc = ngx_get_connection(s); //这是针对新连入用户的连接，和监听套接字 所对应的连接是两个不同的东西
		if (newc == NULL)
		{
			//连接池中连接不够用，那么就得把这个socekt直接关闭并返回了，因为在ngx_get_connection()中已经写日志了，所以这里不需要写日志了
			if (close(s) == -1)
			{
				ngx_log_error_core(NGX_LOG_ALERT, errno, "CSocekt::ngx_event_accept()中close(%d)失败!", s);
			}
			return;
		}

		//成功的拿到了连接池中的一个连接
		memcpy(&newc->s_sockaddr, &mysockaddr, socklen);  //拷贝客户端地址到连接对象【要转成字符串ip地址参考函数ngx_sock_ntop()】
		//{
		//    //测试将收到的地址弄成字符串，格式形如"192.168.1.126:40904"或者"192.168.1.126"
		//    u_char ipaddr[100]; memset(ipaddr,0,sizeof(ipaddr));
		//    ngx_sock_ntop(&newc->s_sockaddr,1,ipaddr,sizeof(ipaddr)-10); //宽度给小点
		//    ngx_log_stderr(0,"ip信息为%s\n",ipaddr);
		//}

		if (!use_accept4)
		{
			//如果不是用accept4()取得的socket，那么就要设置为非阻塞【因为用accept4()的已经被accept4()设置为非阻塞了】
			if (setnonblocking(s) == false)
			{
				//设置非阻塞居然失败
				ngx_close_connection(newc); //关闭socket,这种可以立即回收这个连接，无需延迟，因为其上还没有数据收发，谈不到业务逻辑因此无需延迟；
				return; //直接返回
			}
		}

		newc->listening = oldc->listening;                    //连接对象 和监听对象关联，方便通过连接对象找监听对象【关联到监听端口】
		//newc->w_ready = 1;                                    //标记可以写，新连接写事件肯定是ready的；【从连接池拿出一个连接时这个连接的所有成员都是0】            

		newc->rhandler = &CSocekt::ngx_read_request_handler;  //设置数据来时的读处理函数，其实官方nginx中是ngx_http_wait_request_handler()
		newc->whandler = &CSocekt::ngx_write_request_handler; //设置数据发送时的写处理函数。

		ngx_set_keepalive(s);

		//客户端应该主动发送第一次的数据，这里将读事件加入epoll监控，这样当客户端发送数据来时，会触发ngx_wait_request_handler()被ngx_epoll_process_events()调用        
		if (ngx_epoll_oper_event(
			s,						//socekt句柄
			EPOLL_CTL_ADD,			//事件类型，这里是增加
			EPOLLIN | EPOLLRDHUP,	//标志，这里代表要增加的标志,EPOLLIN：可读，EPOLLRDHUP：TCP连接的远端关闭或者半关闭 ，如果边缘触发模式可以增加 EPOLLET
			0,						//对于事件类型为增加的，不需要这个参数
			newc					//连接池中的连接
		) == -1)
		{
			//增加事件失败，失败日志在ngx_epoll_add_event中写过了，因此这里不多写啥；
			ngx_close_connection(newc);//关闭socket,这种可以立即回收这个连接，无需延迟，因为其上还没有数据收发，谈不到业务逻辑因此无需延迟；
			return; //直接返回
		}
		++m_onlineUserCount;	//连接已建立，断开时在 ngx_close_and_recycle() 中 -1
		/*
		else
		{
			//打印下发送缓冲区大小
			int           n;
			socklen_t     len;
			len = sizeof(int);
			getsockopt(s,SOL_SOCKET,SO_SNDBUF, &n, &len);
			ngx_log_stderr(0,"发送缓冲区的大小为%d!",n); //87040

			n = 0;
			getsockopt(s,SOL_SOCKET,SO_RCVBUF, &n, &len);
			ngx_log_stderr(0,"接收缓冲区的大小为%d!",n); //374400

			int sendbuf = 2048;
			if (setsockopt(s, SOL_SOCKET, SO_SNDBUF,(const void *) &sendbuf,n) == 0)
			{
				ngx_log_stderr(0,"发送缓冲区大小成功设置为%d!",sendbuf);
			}

			 getsockopt(s,SOL_SOCKET,SO_SNDBUF, &n, &len);
			ngx_log_stderr(0,"发送缓冲区的大小为%d!",n); //87040
		}
		*/

		break;
	} while (1);

	return;
}

//worker 中 epoll 初始化之后调用（此时固定占用的 fd 都已打开）：打开预留 fd，确定客户端连接上限
//worker_connections 含监听 socket 占用的连接（与配置文件注释一致）。fd 不够时先把 RLIMIT_NOFILE 软上限抬到硬上限以内，
//仍不够就降低连接上限，保证客户端连接用不光 fd：日志轮转、建队列文件、超限时临时 accept 再关闭都还有余量
void CSocekt::ngx_init_accept_limits()
{
	m_idleFd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	if (m_idleFd == -1)
	{
		ngx_log_error_core(NGX_LOG_ALERT, errno, "CSocekt::ngx_init_accept_limits()中open(/dev/null)失败，fd耗尽时只能暂停接受新连接!");
	}

	int maxClients = m_worker_connections - (int)m_ListenSocketList.size();
	if (maxClients < 1)
		maxClients = 1;

	const int used = CountOpenFds();
	const rlim_t need = (rlim_t)used + NGX_FD_SPARE + (rlim_t)maxClients;
	struct rlimit rl;
	if (getrlimit(RLIMIT_NOFILE, &rl) == -1)
	{
		ngx_log_error_core(NGX_LOG_ALERT, errno, "CSocekt::ngx_init_accept_limits()中getrlimit()失败，客户端连接上限按worker_connections确定!");
		m_maxClientConnections = maxClients;
		return;
	}

	if (rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur < need)
	{
		struct rlimit raised = rl;
		raised.rlim_cur = (rl.rlim_max == RLIM_INFINITY || rl.rlim_max >= need) ? need : rl.rlim_max;
		if (raised.rlim_cur > rl.rlim_cur)
		{
			if (setrlimit(RLIMIT_NOFILE, &raised) == 0)
			{
				ngx_log_error_core(NGX_LOG_NOTICE, 0, "RLIMIT_NOFILE软上限%L抬高到%L", (int64_t)rl.rlim_cur, (int64_t)raised.rlim_cur);
				rl.rlim_cur = raised.rlim_cur;
			}
			else
			{
				ngx_log_error_core(NGX_LOG_ALERT, errno, "CSocekt::ngx_init_accept_limits()中setrlimit()失败!");
			}
		}
	}
	if (rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur < need)
	{
		const int64_t byFd = (int64_t)rl.rlim_cur - used - NGX_FD_SPARE;
		maxClients = byFd < 1 ? 1 : (int)byFd;
		ngx_log_error_core(NGX_LOG_WARN, 0, "进程可打开文件数上限RLIMIT_NOFILE=%L（硬上限%L）不足以支持worker_connections=%d，客户端连接上限降为%d；请调大ulimit -n或systemd的LimitNOFILE",
			(int64_t)rl.rlim_cur, (int64_t)rl.rlim_max, m_worker_connections, maxClients);
	}

	m_maxClientConnections = maxClients;
	ngx_log_error_core(NGX_LOG_NOTICE, 0, "客户端连接上限%d（worker_connections=%d，RLIMIT_NOFILE=%L，已打开fd %d）",
		maxClients, m_worker_connections, (int64_t)rl.rlim_cur, used);
}

//accept 因 fd 或内存耗尽失败（EMFILE/ENFILE/ENOBUFS/ENOMEM），此时连接还留在内核的已完成连接队列里：
//(1)fd 耗尽：关掉预留 fd 腾出一个名额，把排队的连接逐个取出并立即关闭。客户端马上读到 EOF/RST，按连接错误处理，
//   队列清空后监听 socket 不再可读，epoll 线程不会空转
//(2)做不到（预留 fd 已用掉、名额被别的线程抢走、内存不足）：暂停监听事件 NGX_ACCEPT_PAUSE_MS 毫秒，到期再试
void CSocekt::ngx_accept_exhausted(lpngx_connection_t oldc, int err)
{
	int  rejected = 0;
	bool stuck = true;	//队列里还有连接却取不出来，不暂停就会空转

	if ((err == EMFILE || err == ENFILE) && m_idleFd != -1)
	{
		close(m_idleFd);
		m_idleFd = -1;

		int i;
		for (i = 0; i < NGX_ACCEPT_DRAIN_MAX; ++i)
		{
			int s = accept(oldc->fd, NULL, NULL);
			if (s != -1)
			{
				close(s);
				++rejected;
				continue;
			}
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				stuck = false;		//队列已清空
			if (errno != ECONNABORTED && errno != EINTR)
				break;
		}
		if (i == NGX_ACCEPT_DRAIN_MAX)
			stuck = false;			//取满一批，剩下的留给下一轮 epoll

		m_idleFd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	}

	if (stuck)
		ngx_pause_accept();

	unsigned long suppressed;
	if (m_logAcceptExhausted.Allow(MonotonicMs() / 1000, &suppressed))
	{
		ngx_log_error_core(NGX_LOG_CRIT, err, "CSocekt::ngx_event_accept()中accept4()失败，fd或内存耗尽：已拒绝%d个排队连接%s（上次记录后同类失败另有%L次）",
			rejected, stuck ? "，暂停接受新连接" : "", (int64_t)suppressed);
	}
}

//暂停所有监听 socket 的读事件：事件掩码置 0，epoll 不再报告可读；到期由 ngx_accept_wait_timeout() 恢复
void CSocekt::ngx_pause_accept()
{
	if (!m_acceptPaused)
	{
		for (lpngx_listening_t ls : m_ListenSocketList)
			ngx_epoll_oper_event(ls->fd, EPOLL_CTL_MOD, 0, 2, ls->connection);
		m_acceptPaused = true;
	}
	m_acceptResumeAtMs = MonotonicMs() + NGX_ACCEPT_PAUSE_MS;
}

//每次 epoll_wait 之前调用：没暂停则原样返回 timer；暂停未到期则把等待时间限制在剩余时长内；到期则恢复监听事件
int CSocekt::ngx_accept_wait_timeout(int timer)
{
	if (!m_acceptPaused)
		return timer;

	const int64_t left = m_acceptResumeAtMs - MonotonicMs();
	if (left > 0)
		return (timer < 0 || left < timer) ? (int)left : timer;

	if (m_idleFd == -1)
		m_idleFd = open("/dev/null", O_RDONLY | O_CLOEXEC);

	bool resumed = true;
	for (lpngx_listening_t ls : m_ListenSocketList)
	{
		if (ngx_epoll_oper_event(ls->fd, EPOLL_CTL_MOD, EPOLLIN | EPOLLRDHUP, 2, ls->connection) == -1)
			resumed = false;
	}
	if (!resumed)
	{
		//恢复失败（如内核内存不足）：保持暂停，稍后再试，否则监听 socket 会永远收不到新连接
		m_acceptResumeAtMs = MonotonicMs() + NGX_ACCEPT_PAUSE_MS;
		return (timer < 0 || NGX_ACCEPT_PAUSE_MS < timer) ? NGX_ACCEPT_PAUSE_MS : timer;
	}
	m_acceptPaused = false;
	return timer;
}

//对端掉电、拔网线时不会发 FIN/RST，服务端从不主动发数据的空闲连接会一直占着 fd：用 TCP keepalive 探测并回收
//（探测失败后 recv 返回 ETIMEDOUT，连接按出错关闭）
void CSocekt::ngx_set_keepalive(int fd)
{
	if (!m_keepAlive)
		return;

	int on = 1;
	if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &m_keepIdle, sizeof(m_keepIdle)) == -1 ||
		setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &m_keepIntvl, sizeof(m_keepIntvl)) == -1 ||
		setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &m_keepCnt, sizeof(m_keepCnt)) == -1 ||
		setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on)) == -1)
	{
		ngx_log_error_core(NGX_LOG_ALERT, errno, "CSocekt::ngx_set_keepalive()中setsockopt(%d)失败!", fd);
	}
}

