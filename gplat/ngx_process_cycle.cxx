//单进程主流程：主线程跑 epoll 事件循环，线程池 / 发送 / 回收 / 定时器线程是辅助线程

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <iostream>

#include "ngx_func.h"
#include "ngx_macro.h"
#include "ngx_c_conf.h"

#include "ngx_global.h"

static bool ngx_threads_init();

//描述：创建辅助线程并运行事件循环，收到 SIGINT/SIGTERM 后按依赖顺序停止线程后返回
//调用前：配置、日志、QBD、监听端口、信号（ngx_init_signals）已就绪，且还没有创建过线程
void ngx_single_process_cycle()
{
	ngx_log_error_core(NGX_LOG_NOTICE, 0, "gplat %P 启动并开始运行......!", ngx_pid);

	if (ngx_threads_init() == false)
	{
		//内存没释放，但是简单粗暴退出；
		exit(2);
	}

	for (;;)
	{
		ngx_process_events_and_timers(); //处理网络事件和定时器事件

		if (g_stopEvent == 1) //优雅的退出
		{
			ngx_log_stderr(0, "主线程跳出无限循环！");
			break;
		}
	}

	//按依赖关系依次停止：
	//(1) 定时器线程最先停：它的回调（定时器订阅、waitpostdata 超时、延时投递、getresponse 超时）会访问连接对象并调用 msgSend，
	//    若晚于 Shutdown_subproc()，回调就可能碰到已释放的连接对象和已销毁的发送队列互斥量/信号量
	//(2) 线程池：在途请求处理完后退出（之后它们新加的定时器不会再触发）
	//(3) socket：发送/回收线程退出后释放连接池
	g_tm.stop();
	g_threadpool.StopAll();
	g_socket.Shutdown_subproc();
	return;
}

//描述：创建线程池、定时器、发送/回收线程，初始化 epoll
static bool ngx_threads_init()
{
	//线程池代码，率先创建，至少要比和socket相关的内容优先
	CConfig* p_config = CConfig::GetInstance();
	int tmpthreadnums = p_config->GetIntDefault("ProcMsgRecvWorkThreadCount", 5); //处理接收到的消息的线程池中线程数量
	if (g_threadpool.Create(tmpthreadnums) == false)  //创建线程池中线程,创建处理电文的线程
	{
		return false;
	}
	sleep(1);

	//启动定时器的线程，率先创建，至少要比和socket相关的内容优先
	try
	{
		//启动定时器线程
		g_tm.start();

		//启动0.5秒定时器
		g_tm.add_periodic(500, [](void* user) {
			g_socket.NotifyTimerSubscriber("timer_500ms", NULL,0); //通知定时器到达了
			},
			nullptr);

		//启动1秒定时器
		g_tm.add_periodic(1000, [](void* user) {
			g_socket.NotifyTimerSubscriber("timer_1s", NULL, 0); //通知定时器到达了
			},
			nullptr);

		//启动2秒定时器
		g_tm.add_periodic(2000, [](void* user) {
			g_socket.NotifyTimerSubscriber("timer_2s", NULL, 0); //通知定时器到达了
			},
			nullptr);

		//启动3秒定时器
		g_tm.add_periodic(3000, [](void* user) {
			g_socket.NotifyTimerSubscriber("timer_3s", NULL, 0); //通知定时器到达了
			},
			nullptr);

		//启动5秒定时器
		g_tm.add_periodic(5000, [](void* user) {
			g_socket.NotifyTimerSubscriber("timer_5s", NULL, 0); //通知定时器到达了
			},
			nullptr);
	}
	catch (const std::exception& e) {
		std::cerr << "Error: " << e.what() << std::endl;
	}

	//创建发送数据和回收连接的线程
	if (g_socket.Initialize_subproc() == false) //初始化多线程能力相关的信息
	{
		return false;
	}

	//如下这些代码参照官方nginx里的ngx_event_process_init()函数中的代码
	g_socket.ngx_epoll_init();           //初始化epoll相关内容，同时 往监听socket上增加监听事件，从而开始让监听端口履行其职责

	//....将来再扩充代码
	return true;
}
