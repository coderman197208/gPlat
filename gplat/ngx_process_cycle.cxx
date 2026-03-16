//和开启子进程相关

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>   //信号相关头文件 
#include <errno.h>    //errno
#include <unistd.h>

#include "ngx_func.h"
#include "ngx_macro.h"
#include "ngx_c_conf.h"

#include "ngx_global.h"

//函数声明
static void ngx_start_worker_processes(int threadnums);
static int ngx_spawn_process(int threadnums, const char* pprocname);
static void ngx_worker_process_cycle(int inum, const char* pprocname);
static void ngx_worker_process_init(int inum);

//变量声明
static u_char  master_process[] = "master process";

//描述：创建worker子进程
void ngx_master_process_cycle()
{
	sigset_t set;        //信号集

	sigemptyset(&set);   //清空信号集

	//下列这些信号在执行本函数期间不希望收到【考虑到官方nginx中有这些信号，老师就都搬过来了】（保护不希望由信号中断的代码临界区）
	//建议fork()子进程时学习这种写法，防止信号的干扰；
	sigaddset(&set, SIGCHLD);     //子进程状态改变
	sigaddset(&set, SIGALRM);     //定时器超时
	sigaddset(&set, SIGIO);       //异步I/O
	sigaddset(&set, SIGINT);      //终端中断符
	sigaddset(&set, SIGHUP);      //连接断开
	sigaddset(&set, SIGUSR1);     //用户定义信号
	sigaddset(&set, SIGUSR2);     //用户定义信号
	sigaddset(&set, SIGWINCH);    //终端窗口大小改变
	sigaddset(&set, SIGTERM);     //终止
	sigaddset(&set, SIGQUIT);     //终端退出符
	//.........可以根据开发的实际需要往其中添加其他要屏蔽的信号......

	//设置，此时无法接受的信号；阻塞期间，你发过来的上述信号，多个会被合并为一个，暂存着，等你放开信号屏蔽后才能收到这些信号。。。
	//sigprocmask()在第三章第五节详细讲解过
	if (sigprocmask(SIG_BLOCK, &set, NULL) == -1) //第一个参数用了SIG_BLOCK表明设置 进程 新的信号屏蔽字 为 “当前信号屏蔽字 和 第二个参数指向的信号集的并集
	{
		ngx_log_error_core(NGX_LOG_ALERT, errno, "ngx_master_process_cycle()中sigprocmask()失败!");
	}
	//即便sigprocmask失败，程序流程 也继续往下走

	//首先我设置主进程标题---------begin
	size_t size;
	int    i;
	size = sizeof(master_process);  //注意我这里用的是sizeof，所以字符串末尾的\0是被计算进来了的
	size += g_argvneedmem;          //argv参数长度加进来    
	if (size < 1000) //长度小于这个，我才设置标题
	{
		char title[1000] = { 0 };
		strcpy(title, (const char*)master_process); //"master process"
		strcat(title, " ");  //跟一个空格分开一些，清晰    //"master process "
		for (i = 0; i < g_os_argc; i++)         //"master process ./nginx"
		{
			strcat(title, g_os_argv[i]);
		}//end for
		ngx_setproctitle(title); //设置标题
		ngx_log_error_core(NGX_LOG_NOTICE, 0, "%s %P 【master进程】启动并开始运行......!", title, ngx_pid); //设置标题时顺便记录下来进程名，进程id等信息到日志
	}
	//首先我设置主进程标题---------end

	//从配置文件中读取要创建的worker进程数量
	CConfig* p_config = CConfig::GetInstance(); //单例类
	int workprocess = p_config->GetIntDefault("WorkerProcesses", 1); //从配置文件中得到要创建的worker进程数量
	ngx_start_worker_processes(workprocess);  //这里要创建worker子进程

	//创建子进程后，父进程的执行流程会返回到这里，子进程不会走进来    
	sigemptyset(&set); //信号屏蔽字为空，表示不屏蔽任何信号

	//gyb
	close(sockpair[0]);  // 关闭不需要的一端

	for (;; )
	{
		//ngx_log_error_core(0,0,"haha--这是父进程，pid为%P",ngx_pid);

		//sigsuspend是一个原子操作，包含4个步骤：
		//a)根据给定的参数设置新的mask 并 阻塞当前进程【因为是个空集，所以不阻塞任何信号】
		//b)此时，一旦收到信号，便恢复原先的信号屏蔽【我们原来调用sigprocmask()的mask在上边设置的，阻塞了多达10个信号，从而保证我下边的执行流程不会再次被其他信号截断】
		//c)调用该信号对应的信号处理函数
		//d)信号处理函数返回后，sigsuspend返回，使程序流程继续往下走
		//printf("for进来了！\n"); //发现，如果print不加\n，无法及时显示到屏幕上，是行缓存问题，以往没注意；可参考https://blog.csdn.net/qq_26093511/article/details/53255970

		sigsuspend(&set); //阻塞在这里，等待一个信号，此时进程是挂起的，不占用cpu时间，只有收到信号才会被唤醒（返回）；
		//此时master进程完全靠信号驱动干活    
		
		//子进程退出了，发送SIGCHLD信号给父进程，父进程信号处理函数将g_stopEventMain置1，父进程也要退出
		if (g_stopEventMain == 1)
		{
			ngx_log_stderr(0,"主进程跳出无限循环！");
			break;
		}

	}
	return;
}

//描述：根据给定的参数创建指定数量的子进程，因为以后可能要扩展功能，增加参数，所以单独写成一个函数
//threadnums:要创建的子进程数量
static void ngx_start_worker_processes(int threadnums)
{
	int i;
	for (i = 0; i < threadnums; i++)  //master进程在走这个循环，来创建若干个子进程
	{
		ngx_spawn_process(i, "worker process");
	}
	return;
}

//描述：产生一个子进程
//inum：进程编号【0开始】
//pprocname：子进程名字"worker process"
static int ngx_spawn_process(int inum, const char* pprocname)
{
	pid_t  pid;

	pid = fork(); //fork()系统调用产生子进程
	switch (pid)  //pid判断父子进程，分支处理
	{
	case -1: //产生子进程失败
		ngx_log_error_core(NGX_LOG_ALERT, errno, "ngx_spawn_process()fork()产生子进程num=%d,procname=\"%s\"失败!", inum, pprocname);
		return -1;

	case 0:  //子进程分支
		ngx_parent = ngx_pid;              //因为是子进程了，所有原来的pid变成了父pid
		ngx_pid = getpid();                //重新获取pid,即本子进程的pid
		ngx_worker_process_cycle(inum, pprocname);    //我希望所有worker子进程，在这个函数里不断循环着不出来，也就是说，子进程流程不往下边走;

		//如果子进程走到这里，说明ngx_worker_process_cycle()函数里边的循环跳出来了，直接退出子进程
		ngx_log_stderr(0, "子进程退出了");
		exit(0); //子进程退出，返回值为0；
		break;

	default: //这个应该是父进程分支，直接break;，流程往switch之后走            
		break;
	}

	//父进程分支会走到这里，子进程流程不往下边走
	//若有需要，以后再扩展增加其他代码
	return pid;
}

//描述：worker子进程的功能函数，每个woker子进程，就在这里循环着了（无限循环【处理网络事件和定时器事件以对外提供web服务】）
//子进程分叉才会走到这里
//inum：进程编号【0开始】
static void ngx_worker_process_cycle(int inum, const char* pprocname)
{
	close(sockpair[1]);  // 关闭不需要的一端

	//设置一下变量
	ngx_process = NGX_PROCESS_WORKER;  //设置进程的类型，是worker进程

	//重新为子进程设置进程名，不要与父进程重复------
	ngx_worker_process_init(inum);
	ngx_setproctitle(pprocname); //设置标题   
	ngx_log_error_core(NGX_LOG_NOTICE, 0, "%s %P 【worker进程】启动并开始运行......!", pprocname, ngx_pid); //设置标题时顺便记录下来进程名，进程id等信息到日志

	//暂时先放个死循环，我们在这个循环里一直不出来
	//setvbuf(stdout,NULL,_IONBF,0); //这个函数. 直接将printf缓冲区禁止， printf就直接输出了。
	for (;;)
	{
		ngx_process_events_and_timers(); //处理网络事件和定时器事件

		if (g_stopEventChild == 1) //优雅的退出
		{
			ngx_log_stderr(0,"子进程主线程跳出无限循环！");
			break;
		}

	}

	//如果从这个循环跳出来
	g_threadpool.StopAll();      //考虑在这里停止线程池；
	g_socket.Shutdown_subproc(); //socket需要释放的东西考虑释放；
	g_tm.stop();                 //停止定时器线程；
	return;
}

//描述：子进程创建时调用本函数进行一些初始化工作
static void ngx_worker_process_init(int inum)
{
	sigset_t  set;      //信号集

	sigemptyset(&set);  //清空信号集
	if (sigprocmask(SIG_SETMASK, &set, NULL) == -1)  //原来是屏蔽那10个信号【防止fork()期间收到信号导致混乱】，现在不再屏蔽任何信号【接收任何信号】
	{
		ngx_log_error_core(NGX_LOG_ALERT, errno, "ngx_worker_process_init()中sigprocmask()失败!");
	}

	//线程池代码，率先创建，至少要比和socket相关的内容优先
	CConfig* p_config = CConfig::GetInstance();
	int tmpthreadnums = p_config->GetIntDefault("ProcMsgRecvWorkThreadCount", 5); //处理接收到的消息的线程池中线程数量
	if (g_threadpool.Create(tmpthreadnums) == false)  //创建线程池中线程,创建处理电文的线程
	{
		//内存没释放，但是简单粗暴退出；
		exit(-2);
	}
	sleep(1);

	//启动定时器的线程，率先创建，至少要比和socket相关的内容优先
	try
	{
		//启动定时器线程
		g_tm.start();

		//启动0.5秒定时器
		g_tm.add_periodic(500, [](void* user) {
			std::cout << "500毫秒定时器时间到" << std::endl;
			g_socket.NotifyTimerSubscriber("timer_500ms", NULL,0); //通知定时器到达了
			},
			nullptr);

		//启动1秒定时器
		g_tm.add_periodic(1000, [](void* user) {
			std::cout << "1秒定时器时间到" << std::endl;
			g_socket.NotifyTimerSubscriber("timer_1s", NULL, 0); //通知定时器到达了
			},
			nullptr);

		//启动2秒定时器
		g_tm.add_periodic(2000, [](void* user) {
			std::cout << "2秒定时器时间到" << std::endl;
			g_socket.NotifyTimerSubscriber("timer_2s", NULL, 0); //通知定时器到达了
			},
			nullptr);

		//启动3秒定时器
		g_tm.add_periodic(3000, [](void* user) {
			std::cout << "3秒定时器时间到" << std::endl;
			g_socket.NotifyTimerSubscriber("timer_3s", NULL, 0); //通知定时器到达了
			},
			nullptr);

		//启动5秒定时器
		g_tm.add_periodic(5000, [](void* user) {
			std::cout << "5秒定时器时间到" << std::endl;
			g_socket.NotifyTimerSubscriber("timer_5s", NULL, 0); //通知定时器到达了
			},
			nullptr);
	}
	catch (const std::exception& e) {
		std::cerr << "Error: " << e.what() << std::endl;
	}

	//创建发送数据和回收连接的线程
	if (g_socket.Initialize_subproc() == false) //初始化子进程需要具备的一些多线程能力相关的信息
	{
		//内存没释放，但是简单粗暴退出；
		exit(-2);
	}

	//如下这些代码参照官方nginx里的ngx_event_process_init()函数中的代码
	g_socket.ngx_epoll_init();           //初始化epoll相关内容，同时 往监听socket上增加监听事件，从而开始让监听端口履行其职责

	//....将来再扩充代码
	return;
}
