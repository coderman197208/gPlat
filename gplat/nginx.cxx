
//整个程序入口函数

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h> 
#include <errno.h>
#include <arpa/inet.h>

#include "ngx_macro.h"         //各种宏定义
#include "ngx_func.h"          //各种函数声明
#include "ngx_c_conf.h"        //和配置文件处理相关的类,名字带c_表示和类有关
#include "ngx_c_socket.h"      //和socket通讯相关
#include "ngx_c_memory.h"      //和内存分配释放等相关
#include "ngx_c_threadpool.h"  //和多线程有关
#include "ngx_c_slogic.h"      //和socket通讯相关

#include "../include/timer_manager.h"

//本文件用的函数声明
static void freeresource();

static bool gplat_load_qbd();

CLogicSocket   g_socket;        //socket全局对象  
CThreadPool    g_threadpool;    //线程池全局对象
TimerManager   g_tm;			//定时器全局对象(只创建fd，定时器线程在 start() 时才创建)

//和进程本身有关的全局量
pid_t   ngx_pid;                //当前进程的pid

std::atomic<int> g_stopEvent{ 0 };	//退出标志,0不退出1退出（主线程置位，其它线程会读，必须是原子的）

//程序主入口函数
int main(int argc, char* const* argv)
{
	int exitcode = 0;           //退出代码，先给0表示正常退出

	(void)argc;
	(void)argv;

	//(0)先初始化的变量
	g_stopEvent = 0;				//标记是否退出，0不退出

	//(1)无伤大雅也不需要释放的放最上边    
	ngx_pid = getpid();				//取得进程pid

	//全局量有必要初始化的
	ngx_log.fd = -1;					//-1：表示日志文件尚未打开；因为后边ngx_log_stderr要用所以这里先给-1
	//(2)初始化失败，就要直接退出的
	//配置文件必须最先要，后边初始化啥的都用，所以先把配置读出来，供后续使用 
	CConfig* p_config = CConfig::GetInstance(); //单例类
	if (p_config->Load("../config/gplat.conf") == false)	//把配置文件内容载入到内存            
	{
		ngx_log_init();    //初始化日志
		ngx_log_stderr(0, "配置文件[%s]载入失败，退出!", "gplat.conf");
		//exit(1);终止进程，在main中出现和return效果一样 ,exit(0)表示程序正常, exit(1)/exit(-1)表示程序异常退出，exit(2)表示表示系统找不到指定的文件
		exitcode = 2; //标记找不到文件
		goto lblexit;
	}
	//(2.1)内存单例类可以在这里初始化，返回值不用保存
	CMemory::GetInstance();

	//(3)一些必须事先准备好的资源，先初始化
	ngx_log_init();					//日志初始化(创建/打开日志文件)，这个需要配置项，所以必须放配置文件载入的后边；

	//单进程多线程，不再有 worker 进程；旧配置里的 WorkerProcesses 不再生效
	if (p_config->GetIntDefault("WorkerProcesses", 1) != 1)
	{
		ngx_log_error_core(NGX_LOG_WARN, 0, "配置项WorkerProcesses已废弃，gplat 是单进程多线程，该值被忽略");
	}

	//(3.1)gyb加载qbd，需要在配置文件和日志初始化之后
	if (gplat_load_qbd() == false)
	{
		ngx_log_stderr(0, "加载qbd失败，退出!");
		exitcode = 1;
		goto lblexit;
	}

	//(4)监听端口
	if (g_socket.Initialize() == false)//初始化socket
	{
		exitcode = 1;
		goto lblexit;
	}

	//(5)创建守护进程：必须在创建任何线程之前（fork 之后只有调用线程存活）
	if (p_config->GetIntDefault("Daemon", 0) == 1) //读配置文件，拿到配置文件中是否按守护进程方式启动的选项
	{
		//1：按守护进程方式运行
		int cdaemonresult = ngx_daemon();
		if (cdaemonresult == -1)	//fork()失败
		{
			exitcode = 1;			//标记失败
			goto lblexit;
		}
		if (cdaemonresult == 1)
		{
			//这是原始的父进程
			freeresource();   //只有进程退出了才goto到 lblexit，用于提醒用户进程退出了
			//而我现在这个情况属于正常fork()守护进程后的正常退出，不应该跑到lblexit()去执行，因为那里有一条打印语句标记整个进程的退出，这里不该限制该条打印语句；
			exitcode = 0;
			return exitcode;  //整个进程直接在这里退出
		}
		//走到这里，成功创建了守护进程并且这里已经是fork()出来的进程，它就是唯一的服务进程
	}

	//(6)信号：屏蔽退出类信号并交给 signalfd，必须在创建线程之前，线程才会继承信号屏蔽字
	if (ngx_init_signals() != 0)
	{
		exitcode = 1;
		goto lblexit;
	}

	//(7)开始正式的主工作流程：主线程在 epoll 循环里处理事件，直到收到 SIGINT/SIGTERM
	ngx_single_process_cycle();
	ngx_close_signals();

lblexit:
	//(5)该释放的资源要释放掉
	ngx_log_stderr(0, "程序退出，再见了!");
	freeresource();
	return exitcode;
}

//专门在程序执行末尾释放资源的函数【一系列的main返回前的释放动作函数】
void freeresource()
{
	//关闭日志文件
	if (ngx_log.fd != STDERR_FILENO && ngx_log.fd != -1)
	{
		close(ngx_log.fd); //不用判断结果了
		ngx_log.fd = -1; //标记下，防止被再次close吧
	}
	pthread_mutex_destroy(&ngx_log.log_mutex); //销毁日志互斥量
}

//gyb
#include <filesystem>
#include <vector>
#include <string>
#include <iostream>
#include "../include/higplat.h"

namespace fs = std::filesystem;

//gyb
std::vector<std::string> getFilesInDirectory(const std::string& directoryPath) {
	std::vector<std::string> files;

	try {
		for (const auto& entry : fs::directory_iterator(directoryPath)) {
			if (entry.is_regular_file()) {
				files.push_back(entry.path().filename().string());
			}
		}
	}
	catch (const fs::filesystem_error& e) {
		std::cerr << "文件系统错误: " << e.what() << std::endl;
	}

	return files;
}

//gyb
bool gplat_load_qbd()
{
	//从配置文件读取QBD文件目录路径，默认./qbdfile
	CConfig* p_config = CConfig::GetInstance();
	const char* confPath = p_config->GetString("QbdFilePath");
	std::string directoryPath = confPath ? confPath : "./qbdfile";

	//检查目录是否存在
	if (!fs::is_directory(directoryPath)) {
		ngx_log_stderr(0, "QBD目录[%s]不存在，退出!", directoryPath.c_str());
		return false;
	}

	//通过API设置higplat的QBD文件路径
	SetQbdPath(directoryPath.c_str());

	auto files = getFilesInDirectory(directoryPath);

	std::cout << "目录 " << directoryPath << " 中的文件:" << std::endl;
	for (const auto& file : files) {
		if (file == "_qbd_file_directory") {
			continue; // 跳过指定文件
		}

		if (LoadQ(file.c_str()) == false) //把qbd文件内容载入到内存            
		{
			ngx_log_stderr(0, "文件[%s]载入失败，退出!", file.c_str());
			return false;
		}

		std::cout << file << " loaded" << std::endl;
	}
	return true;
}