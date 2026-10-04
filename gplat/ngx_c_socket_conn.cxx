
//和网络中连接/连接池有关的函数

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

#include "ngx_c_conf.h"
#include "ngx_macro.h"
#include "ngx_global.h"
#include "ngx_func.h"
#include "ngx_c_socket.h"
#include "ngx_c_memory.h"
#include "ngx_c_lockmutex.h"

ngx_connection_s::ngx_connection_s()
{
	iCurrsequence = 0;
	iSendQueued = 0;
	pthread_mutex_init(&logicPorcMutex, NULL); //互斥量初始化
	pthread_mutex_init(&sendMutex, NULL);
}

ngx_connection_s::~ngx_connection_s()
{
	pthread_mutex_destroy(&logicPorcMutex);    //互斥量释放
	pthread_mutex_destroy(&sendMutex);
}

//分配出去一个连接的时候初始化一些内容,原来内容放在 ngx_get_connection()里，现在放在这里
void ngx_connection_s::GetOneToUse()
{
	CLock sendLock(&sendMutex);

	++iCurrsequence;

	curStat = _PKG_HD_INIT;                   //收包状态处于 初始状态，准备接收数据包头【状态机】
	precvbuf = dataHeadInfo;                  //收包我要先收到这里来，因为我要先收包头，所以收数据的buff直接就是dataHeadInfo

	//irecvlen = sizeof(COMM_PKG_HEADER);     //这里指定收数据的长度，这里先要求收包头这么长字节的数据
	irecvlen = sizeof(MSGHEAD);               //这里指定收数据的长度，这里先要求收包头这么长字节的数据

	precvMemPointer = NULL;                   //既然没new内存，那自然指向的内存地址先给NULL
	iThrowsendCount = 0;                      //原子的
	iSendQueued = 0;                          //旧连接残留在发送队列里的消息序号已过期，会被直接丢弃且不再计数
	psendMemPointer = NULL;                   //发送数据头指针记录
	events = 0;                               //epoll事件先给0 

	m_bWaitingPost = false;
	m_bWaitingTimeout = false;
	m_timerID = -1;
	m_droppedPost = 0;
	m_lastDropLog = 0;
}

//回收一个连接的时候做一些事
void ngx_connection_s::PutOneToFree()
{
	CLock logicLock(&logicPorcMutex);
	// 断开后仍在途的通知可能又入队了 POST
	ClearPostList();

	CLock sendLock(&sendMutex);

	++iCurrsequence;
	if (precvMemPointer != NULL)//我们曾经给这个连接分配过接收数据的内存，则要释放内存
	{
		CMemory::GetInstance()->FreeMemory(precvMemPointer);
		precvMemPointer = NULL;
	}
	if (psendMemPointer != NULL) //如果发送数据的缓冲区里有内容，则要释放内存
	{
		CMemory::GetInstance()->FreeMemory(psendMemPointer);
		psendMemPointer = NULL;
	}

	iThrowsendCount = 0;		//设置不设置感觉都行 

	//gyb
	this->ClearTagList();
}

void ngx_connection_s::ClearPostList()
{
	CMemory* p_memory = CMemory::GetInstance();
	for (char* p : m_listPost)
	{
		p_memory->FreeMemory(p);
	}
	m_listPost.clear();
}

bool ngx_connection_s::EnqueuePost(char* postbuf, const char* tagname)
{
	if ((int)m_listPost.size() < s_maxPendingPost.load(std::memory_order_relaxed))
	{
		m_listPost.push_back(postbuf);
		return true;
	}

	CMemory::GetInstance()->FreeMemory(postbuf);
	++m_droppedPost;
	time_t now = time(NULL);
	if (now - m_lastDropLog >= 5) // 风暴期间限频，避免日志本身成为负担
	{
		m_lastDropLog = now;
		ngx_log_stderr(0, "WARN:连接待发事件队列已满(%d)，丢弃最新事件 tag=%s，本连接累计丢弃=%lu，请检查是否存在事件风暴或订阅者不取事件",
			s_maxPendingPost.load(), tagname ? tagname : "", m_droppedPost);
	}
	return false;
}

//初始化连接池
void CSocekt::initconnection()
{
	lpngx_connection_t p_Conn;
	CMemory* p_memory = CMemory::GetInstance();

	int ilenconnpool = sizeof(ngx_connection_t);
	for (int i = 0; i < m_worker_connections; ++i) //先创建这么多个连接，后续不够再增加
	{
		p_Conn = (lpngx_connection_t)p_memory->AllocMemory(ilenconnpool, true); //清理内存 , 因为这里分配内存new char，无法执行构造函数，所以如下：
		//手工调用构造函数，因为AllocMemory里无法调用构造函数
		p_Conn = new(p_Conn) ngx_connection_t();	//定位new，释放则显式调用p_Conn->~ngx_connection_t();		
		p_Conn->GetOneToUse();
		m_connectionList.push_back(p_Conn);			//所有连接【不管是否空闲】都放在这个list
		m_freeconnectionList.push_back(p_Conn);		//空闲连接会放在这个list
	}
	m_free_connection_n = m_total_connection_n = m_connectionList.size(); //开始这两个列表一样大
	return;
}

//最终回收连接池，释放内存
void CSocekt::clearconnection()
{
	lpngx_connection_t p_Conn;
	CMemory* p_memory = CMemory::GetInstance();

	while (!m_connectionList.empty())
	{
		p_Conn = m_connectionList.front();
		m_connectionList.pop_front();
		p_Conn->~ngx_connection_t();     //手工调用析构函数
		p_memory->FreeMemory(p_Conn);
	}
}

//从连接池中获取一个空闲连接【当一个客户端连接TCP进入，我希望把这个连接和连接池中的一个连接对象绑到一起，后续可以通过这个连接，把这个对象拿到，因为对象里边可以记录各种信息】
lpngx_connection_t CSocekt::ngx_get_connection(int isock)
{
	//因为可能有其他线程要访问m_freeconnectionList，比如可能有专门的释放线程要释放/或者主线程要释放之类的，所以对m_connectionList的访问应该保护一下
	CLock lock(&m_connectionMutex);

	if (!m_freeconnectionList.empty())
	{
		//有空闲的，自然是从空闲的中摘取
		lpngx_connection_t p_Conn = m_freeconnectionList.front(); //返回第一个元素但不检查元素存在与否
		m_freeconnectionList.pop_front();                         //移除第一个元素但不返回	
		p_Conn->GetOneToUse();
		--m_free_connection_n;
		p_Conn->fd = isock;
		return p_Conn;
	}

	//走到这里，表示没空闲的连接了，那就考虑重新创建一个连接
	CMemory* p_memory = CMemory::GetInstance();
	lpngx_connection_t p_Conn = (lpngx_connection_t)p_memory->AllocMemory(sizeof(ngx_connection_t), true);
	p_Conn = new(p_Conn) ngx_connection_t();
	p_Conn->GetOneToUse();
	m_connectionList.push_back(p_Conn); //写入到连接总表中来，但不能入到空闲表中来，因为凡是调这个函数的，肯定是要立刻使用这个连接的
	++m_total_connection_n;
	p_Conn->fd = isock;
	return p_Conn;
}

//归还参数pConn所代表的连接到到连接池中，注意参数类型是lpngx_connection_t
void CSocekt::ngx_free_connection(lpngx_connection_t pConn)
{
	CLock lock(&m_connectionMutex);

	pConn->PutOneToFree();

	//扔到空闲连接列表里
	m_freeconnectionList.push_back(pConn);

	//空闲连接数+1
	++m_free_connection_n;

	return;
}


//将要回收的连接放到一个队列中来，后续有专门的线程会处理这个队列中的连接的回收
//有些连接，我们不希望马上释放，要隔一段时间后再释放以确保服务器的稳定，所以，我们把这种隔一段时间才释放的连接先放到一个队列中来
void CSocekt::inRecyConnectQueue(lpngx_connection_t pConn)
{
	CLock lock(&m_recyconnqueueMutex); //针对连接回收列表的互斥量，因为线程ServerRecyConnectionThread()也要访问回收列表；

	pConn->inRecyTime = time(NULL);        //记录回收时间
	m_recyconnectionList.push_back(pConn); //等待ServerRecyConnectionThread线程自会处理 
	++m_totol_recyconnection_n;            //待释放连接队列大小+1
	return;
}

//对端断开或出错时调用：关闭 socket 并放入回收队列
//关闭和序号递增必须在 sendMutex 内完成，否则其他线程可能在已关闭、甚至已被新连接复用的 fd 上 send()
void CSocekt::ngx_close_and_recycle(lpngx_connection_t pConn)
{
	{
		CLock sendLock(&pConn->sendMutex);
		if (close(pConn->fd) == -1)
		{
			ngx_log_error_core(NGX_LOG_ALERT, errno, "CSocekt::ngx_close_and_recycle()中close(%d)失败!", pConn->fd);
		}
		++pConn->iCurrsequence;

		//fd 已关闭，不会再有 EPOLLOUT，等待续发的数据直接丢弃
		if (pConn->iThrowsendCount > 0)
		{
			CMemory::GetInstance()->FreeMemory(pConn->psendMemPointer);
			pConn->psendMemPointer = NULL;
			pConn->iThrowsendCount = 0;
		}
	}
	inRecyConnectQueue(pConn);
}

//处理连接回收的线程
void* CSocekt::ServerRecyConnectionThread(void* threadData)
{
	ThreadItem* pThread = static_cast<ThreadItem*>(threadData);
	CSocekt* pSocketObj = pThread->_pThis;

	time_t currtime;
	int err;
	std::list<lpngx_connection_t>::iterator pos, posend;
	lpngx_connection_t p_Conn;

	while (1)
	{
		//为简化问题，我们直接每次休息200毫秒
		usleep(200 * 1000);  //单位是微妙,又因为1毫秒=1000微妙，所以 200 *1000 = 200毫秒

		//不管啥情况，先把这个条件成立时该做的动作做了
		if (pSocketObj->m_totol_recyconnection_n > 0)
		{
			currtime = time(NULL);
			err = pthread_mutex_lock(&pSocketObj->m_recyconnqueueMutex);
			if (err != 0) ngx_log_stderr(err, "CSocekt::ServerRecyConnectionThread()中pthread_mutex_lock()失败，返回的错误码为%d!", err);

		lblRRTD:
			pos = pSocketObj->m_recyconnectionList.begin();
			posend = pSocketObj->m_recyconnectionList.end();
			for (; pos != posend; ++pos)
			{
				p_Conn = (*pos);
				if (
					((p_Conn->inRecyTime + pSocketObj->m_RecyConnectionWaitTime) > currtime) && (g_stopEventChild == 0) //如果不是要整个系统退出，你可以continue，否则就得要强制释放
					)
				{
					continue; //没到释放的时间
				}
				//到释放的时间了: 
				//这将来可能还要做一些是否能释放的判断[在我们写完发送数据代码之后吧]，先预留位置


				//我认为，凡是到释放时间的，iThrowsendCount都应该为0；这里我们加点日志判断下
				if (p_Conn->iThrowsendCount != 0)
				{
					//这确实不应该，打印个日志吧；
					ngx_log_stderr(0, "CSocekt::ServerRecyConnectionThread()中到释放时间却发现p_Conn.iThrowsendCount!=0，这个不该发生");
				}

				//流程走到这里，表示可以释放，那我们就开始释放
				--pSocketObj->m_totol_recyconnection_n;        //待释放连接队列大小-1
				pSocketObj->m_recyconnectionList.erase(pos);   //迭代器已经失效，但pos所指内容在p_Conn里保存着呢

				//ngx_log_stderr(0,"CSocekt::ServerRecyConnectionThread()执行，连接%d被归还.",p_Conn->fd);

				pSocketObj->ngx_free_connection(p_Conn);	   //归还参数pConn所代表的连接到到连接池中
				goto lblRRTD;
			}
			err = pthread_mutex_unlock(&pSocketObj->m_recyconnqueueMutex);
			if (err != 0)  ngx_log_stderr(err, "CSocekt::ServerRecyConnectionThread()pthread_mutex_unlock()失败，返回的错误码为%d!", err);
		}

		if (g_stopEventChild == 1) //要退出整个程序，那么肯定要先退出这个循环
		{
			if (pSocketObj->m_totol_recyconnection_n > 0)
			{
				//因为要退出，所以就得硬释放了，不管到没到时间，不管有没有其他不 允许释放的需求，都得硬释放
				err = pthread_mutex_lock(&pSocketObj->m_recyconnqueueMutex);
				if (err != 0) ngx_log_stderr(err, "CSocekt::ServerRecyConnectionThread()中pthread_mutex_lock2()失败，返回的错误码为%d!", err);

			lblRRTD2:
				pos = pSocketObj->m_recyconnectionList.begin();
				posend = pSocketObj->m_recyconnectionList.end();
				for (; pos != posend; ++pos)
				{
					p_Conn = (*pos);
					--pSocketObj->m_totol_recyconnection_n;        //待释放连接队列大小-1
					pSocketObj->m_recyconnectionList.erase(pos);   //迭代器已经失效，但pos所指内容在p_Conn里保存着呢
					pSocketObj->ngx_free_connection(p_Conn);	   //归还参数pConn所代表的连接到到连接池中
					goto lblRRTD2;
				} //end for
				err = pthread_mutex_unlock(&pSocketObj->m_recyconnqueueMutex);
				if (err != 0)  ngx_log_stderr(err, "CSocekt::ServerRecyConnectionThread()pthread_mutex_unlock2()失败，返回的错误码为%d!", err);
			}
			break; //整个程序要退出了
		}
	} //end while    

	return (void*)0;
}

//用户连入，我们accept4()时，得到的socket在处理中产生失败，则资源用这个函数释放【因为这里涉及到好几个要释放的资源，所以写成函数】
//我们把ngx_close_accepted_connection()函数改名为让名字更通用，并从文件ngx_socket_accept.cxx迁移到本文件中，并改造其中代码，注意顺序
void CSocekt::ngx_close_connection(lpngx_connection_t pConn)
{
	//pConn->fd = -1; //官方nginx这么写，不要这个东西，回收时不要轻易动连接里边的内容
	ngx_free_connection(pConn);
	if (close(pConn->fd) == -1)
	{
		ngx_log_error_core(NGX_LOG_ALERT, errno, "CSocekt::ngx_close_connection()中close(%d)失败!", pConn->fd);
	}
	return;
}
