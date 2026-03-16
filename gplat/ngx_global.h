#ifndef __NGX_GBLDEF_H__
#define __NGX_GBLDEF_H__

#include <signal.h>
#include <pthread.h>

#include "ngx_c_slogic.h"
#include "ngx_c_threadpool.h"

//一些比较通用的定义放在这里，比如typedef定义
//一些全局变量的外部声明也放在这里

//类型定义----------------

//结构定义
typedef struct _CConfItem
{
	char ItemName[50];
	char ItemContent[500];
}CConfItem,*LPCConfItem;

//和运行日志相关
typedef struct
{
	int                log_level;           //日志级别 或者日志类型，ngx_macro.h里分0-8共9个级别
	int                fd;                  //日志文件描述符
	char               log_path[256];       //日志文件路径，轮转时需要用到
	int                log_rotate_count;    //保留的备份文件数量，0=不轮转
	off_t              log_rotate_size;     //单个日志文件最大字节数，超过则轮转
	pthread_mutex_t    log_mutex;           //日志写入互斥量，保护轮转+写入
}ngx_log_t;

//外部全局量声明
extern size_t        g_argvneedmem;
extern size_t        g_envneedmem; 
extern int           g_os_argc; 
extern char          **g_os_argv;
extern char          *gp_envmem; 
extern int           g_daemonized;
extern CLogicSocket  g_socket;  
extern CThreadPool   g_threadpool;

extern pid_t         ngx_pid;
extern pid_t         ngx_parent;
extern ngx_log_t     ngx_log;
extern int           ngx_process;   
extern sig_atomic_t  ngx_reap;   
extern int           g_stopEventChild;
extern int           g_stopEventMain;

extern int sockpair[2]; //socketpair的文件描述符，父进程和子进程之间的通信管道

#endif
