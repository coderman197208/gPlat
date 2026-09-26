
// 网络以及逻辑处理有关

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>	   //uintptr_t
#include <stdarg.h>	   //va_start....
#include <unistd.h>	   //STDERR_FILENO等
#include <sys/time.h>  //gettimeofday
#include <time.h>	   //localtime_r
#include <fcntl.h>	   //open
#include <errno.h>	   //errno
#include <sys/ioctl.h> //ioctl
#include <arpa/inet.h>
#include <pthread.h> //多线程
#include <iostream>
#include <algorithm>

#include "ngx_c_conf.h"
#include "ngx_macro.h"
#include "ngx_global.h"
#include "ngx_func.h"
#include "ngx_c_memory.h"
#include "ngx_c_slogic.h"
#include "ngx_logiccomm.h"
#include "ngx_c_lockmutex.h"

#include "../include/higplat.h"

thread_local char g_buffer[MAXMSGLEN] = {0};

// 定义成员函数指针
typedef bool (CLogicSocket::*handler)(lpngx_connection_t pConn,		 // 连接池中连接的指针
									  LPSTRUC_MSG_HEADER pMsgHeader, // 消息头指针
									  char *pPkgBody,				 // 包体指针
									  unsigned short iBodyLength);	 // 包体长度

// gyb
// 用来保存成员函数指针的数组
// 顺序必须和msg.h中的MSGID对应
static const handler statusHandler[] =
	{
		// 数组前5个元素，保留，以备将来增加一些基本服务器功能
		NULL, // 【0】：下标从0开始
		NULL, // 【1】
		NULL, // 【2】
		NULL, // 【3】
		NULL, // 【4】

		// 以下是处理具体的业务逻辑
		&CLogicSocket::noop,				  // SUCCEED = 5
		&CLogicSocket::noop,				  // FAIL
		&CLogicSocket::noop,				  // CONNECT
		&CLogicSocket::noop,				  // RECONNECT
		&CLogicSocket::noop,				  // DISCONNECT
		&CLogicSocket::noop,				  // OPEN
		&CLogicSocket::noop,				  // OPENQ, OPENB
		&CLogicSocket::noop,				  // CLOSEQ, CLOSEB
		&CLogicSocket::HandleClearQ,		  // CLEARQ
		&CLogicSocket::noop,				  // ISEMPTYQ
		&CLogicSocket::noop,				  // ISFULLQ
		&CLogicSocket::HandleReadQ,			  // READQ
		&CLogicSocket::noop,				  // PEEKQ
		&CLogicSocket::HandleWriteQ,		  // WRITEQ
		&CLogicSocket::HandleReadB,			  // READB
		&CLogicSocket::HandleReadBString,	  // READBSTRING
		&CLogicSocket::HandleWriteB,		  // WRITEB
		&CLogicSocket::noop,				  // QDATA
		&CLogicSocket::noop,				  // READHEAD
		&CLogicSocket::noop,				  // MULREADQ
		&CLogicSocket::noop,				  // SETPTRQ
		&CLogicSocket::noop,				  // WATCHDOG
		&CLogicSocket::noop,				  // SELECTTB
		&CLogicSocket::noop,				  // CLEARTB
		&CLogicSocket::noop,				  // INSERTTB
		&CLogicSocket::noop,				  // REFRESHTB
		&CLogicSocket::HandleReadType,		  // READTYPE
		&CLogicSocket::HandleCreateItem,	  // CREATEITEM
		&CLogicSocket::noop,				  // CREATETABLE
		&CLogicSocket::HandleDeleteItem,	  // DELETEITEM
		&CLogicSocket::noop,				  // DELETETABLE
		&CLogicSocket::noop,				  // READHEADB
		&CLogicSocket::noop,				  // READHEADDB
		&CLogicSocket::noop,				  // ACK
		&CLogicSocket::noop,				  // POPARECORDQ
		&CLogicSocket::HandleWriteBString,	  // WRITEBSTRING
		&CLogicSocket::noop,				  // WRITETOL1
		&CLogicSocket::HandleSubscribe,		  // SUBSCRIBE
		&CLogicSocket::noop,				  // CANCELSUBSCRIBE
		&CLogicSocket::noop,				  // POST
		&CLogicSocket::HandlePostWait,		  // POSTWAIT
		&CLogicSocket::noop,				  // PASSTOSERVER
		&CLogicSocket::HandleClearB,		  // CLEARB
		&CLogicSocket::noop,				  // CLEARDB
		&CLogicSocket::HandleRegisterPlcServer, // REGISTERPLCSERVER
		&CLogicSocket::HandleWriteBPlc,		  // WRITEBPLC
		&CLogicSocket::HandleWriteBStringPlc, // WRITEBSTRINGPLC
		&CLogicSocket::HandleReadBoardInfo,   // READBOARDINFO
		&CLogicSocket::HandleCreateQueue,   // CREATEQUEUE
		&CLogicSocket::HandleGetResponse,   // GETRESPONSE
};

#define AUTH_TOTAL_COMMANDS sizeof(statusHandler) / sizeof(handler) // 整个数组有多少个命令

// 构造函数
CLogicSocket::CLogicSocket()
{
}

// 析构函数
CLogicSocket::~CLogicSocket()
{
}

// 初始化函数【fork()子进程之前执行】
// 成功返回true，失败返回false
bool CLogicSocket::Initialize()
{
	// 这里可以做一些和本类相关的初始化工作

	bool bParentInit = CSocekt::Initialize(); // 调用父类的同名函数
	return bParentInit;
}

// 处理收到的数据包，由线程池来调用本函数
// pMsgBuf：消息头 + 包头 + 包体
void CLogicSocket::threadRecvProcFunc(char *pMsgBuf)
{
	LPSTRUC_MSG_HEADER pMsgHeader = (LPSTRUC_MSG_HEADER)pMsgBuf; // 消息头
	PMSGHEAD pPkgHeader = (PMSGHEAD)(pMsgBuf + m_iLenMsgHeader); // 包头
	[[maybe_unused]] void *pPkgBody;												 // 指向包体的指针

	unsigned short pkglen = pPkgHeader->bodysize; // 客户端指明的包大小【包体】,不含包头

	unsigned short imsgCode = pPkgHeader->id;	   // 消息代码拿出来
	lpngx_connection_t p_Conn = pMsgHeader->pConn; // 消息头中藏着连接池中连接的指针

	// 如果从收到客户端发送来的包，到服务器释放一个线程池中的线程处理该包的过程中，客户端断开了，这种包就不必处理了
	if (p_Conn->iCurrsequence != pMsgHeader->iCurrsequence) // 该连接池中连接以被其他tcp连接【其他socket】占用，这说明原来的客户端和本服务器的连接断了，这种包直接丢弃
	{
		return; // 丢弃不处理【客户端断开了】
	}

	// 判断消息码是正确的，防止客户端恶意发送一个不在我们服务器处理范围内的消息码
	if (imsgCode >= AUTH_TOTAL_COMMANDS)
	{
		ngx_log_stderr(0, "CLogicSocket::threadRecvProcFunc()中imsgCode=%d消息码不对!", imsgCode);
		return; // 丢弃不理这种包【恶意包或者错误包】
	}

	// 继续判断是否有相应的处理函数
	if (statusHandler[imsgCode] == NULL) // 这种用imsgCode的方式可以使查找要执行的成员函数效率特别高
	{
		ngx_log_stderr(0, "CLogicSocket::threadRecvProcFunc()中imsgCode=%d消息码找不到对应的处理函数!", imsgCode);
		return;
	}

	// 调用消息码对应的成员函数来处理
	(this->*statusHandler[imsgCode])(p_Conn, pMsgHeader, (char *)pPkgHeader, pkglen); // pkglen只是包体长度，不包含包头
	return;
}

bool CLogicSocket::noop(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char *pPkgBody, unsigned short iBodyLength)
{
	ngx_log_stderr(0, "执行了CLogicSocket::noop()!");
	return true;
}

bool CLogicSocket::HandleReadQ(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char *pPkgHeader, unsigned short iBodyLength)
{
	// 首先判断包体的合法性
	if (pPkgHeader == NULL)
	{
		return false;
	}

	PPKGHEAD pPkgHead = (PPKGHEAD)pPkgHeader; // 包头
	bool ret;
	char ip[16];
	int iLenPkgBody = pPkgHead->datasize;

	// 分配返回数据需要的内存
	CMemory *p_memory = CMemory::GetInstance();
	char *p_sendbuf = (char *)p_memory->AllocMemory(m_iLenMsgHeader + m_iLenPkgHeader + iLenPkgBody, false); // 准备发送的格式，这里是消息头+包头+包体
	// 在 if 语句条件判断中直接进行复制操作 会导致编译器警告：suggest parentheses around assignment used as truth value
	// 解决方案：避免这样写；或者使用 (()) 向编译器明确表达式的意图
	if ((ret = ReadQ(pPkgHead->qname, p_sendbuf + m_iLenMsgHeader + m_iLenPkgHeader, iLenPkgBody, ip)))
	{
		pPkgHead->error = 0;
		pPkgHead->bodysize = pPkgHead->datasize;
		strcpy(pPkgHead->ip, ip);
	}
	else
	{
		pPkgHead->error = GetLastErrorQ();
		pPkgHead->bodysize = 0;
	}

	// mark 有必要互斥吗？写入发送队列m_MsgSendQueue的时候已经互斥了，这里又不是真正的发送线程
	CLock lock(&pConn->logicPorcMutex);

	// 填充消息头
	memcpy(p_sendbuf, pMsgHeader, m_iLenMsgHeader);
	// 填充包头
	memcpy(p_sendbuf + m_iLenMsgHeader, pPkgHeader, m_iLenPkgHeader);

	// 发送数据包
	msgSend(p_sendbuf);

	return true;
}

bool CLogicSocket::HandleWriteQ(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char *pPkgHeader, unsigned short iBodyLength)
{
	// 判断包体的合法性
	if (pPkgHeader == NULL)
	{
		return false;
	}

	PPKGHEAD pPkgHead = (PPKGHEAD)pPkgHeader; // 包头
	bool ret;
	[[maybe_unused]] char *data = (char *)pPkgHead + sizeof(PKGHEAD);
	if ((ret = WriteQ(pPkgHead->qname, (char *)pPkgHead + sizeof(PKGHEAD), pPkgHead->datasize)))
	{
		pPkgHead->error = 0;
	}
	else
	{
		pPkgHead->error = GetLastErrorQ();
	}

	pPkgHead->bodysize = 0;

	{
		CLock lock(&pConn->logicPorcMutex); // 凡是和本用户有关的访问都互斥

		CMemory *p_memory = CMemory::GetInstance();
		char *p_sendbuf = (char *)p_memory->AllocMemory(m_iLenMsgHeader + m_iLenPkgHeader, false); // 准备发送的格式，这里是消息头+包头+包体
		// 填充消息头
		memcpy(p_sendbuf, pMsgHeader, m_iLenMsgHeader); // 消息头直接拷贝到这里来
		// 填充包头
		memcpy(p_sendbuf + m_iLenMsgHeader, pPkgHeader, m_iLenPkgHeader); // 包头直接拷贝到这里来

		// 发送数据包
		msgSend(p_sendbuf);
	}

	pPkgHead->bodysize = pPkgHead->datasize; // 为了发布订阅的时候能拿到正确的包体长度，实际没用到

	// 发布订阅
	if (ret)
	{
		strcpy(pPkgHead->itemname, pPkgHead->qname); // 必须的，因为最终发布事件的时候是用的itemname
		NotifySubscriber(pPkgHead->itemname, (char *)pPkgHead + sizeof(PKGHEAD), pPkgHead->datasize);
	}

	return true;
}

bool CLogicSocket::HandleClearQ(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char *pPkgHeader, unsigned short iBodyLength)
{
	if (pPkgHeader == NULL)
	{
		return false;
	}

	PPKGHEAD pPkgHead = (PPKGHEAD)pPkgHeader; // 包头
	bool ret;

	if ((ret = ClearQ(pPkgHead->qname)))
	{
		pPkgHead->error = 0;
	}
	else
	{
		pPkgHead->error = GetLastErrorQ();
	}

	pPkgHead->bodysize = 0;

	CMemory *p_memory = CMemory::GetInstance();
	char *p_sendbuf = (char *)p_memory->AllocMemory(m_iLenMsgHeader + m_iLenPkgHeader, false); // 准备发送的格式，这里是消息头+包头+包体
	// 填充消息头
	memcpy(p_sendbuf, pMsgHeader, m_iLenMsgHeader); // 消息头直接拷贝到这里来
	// 填充包头
	memcpy(p_sendbuf + m_iLenMsgHeader, pPkgHeader, m_iLenPkgHeader); // 包头直接拷贝到这里来

	// 发送数据包
	msgSend(p_sendbuf);

	return true;
}

bool CLogicSocket::HandleReadB(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char *pPkgHeader, unsigned short iBodyLength)
{
	if (pPkgHeader == NULL)
	{
		return false;
	}

	timespec timestamp;

	PPKGHEAD pPkgHead = (PPKGHEAD)pPkgHeader; // 包头
	bool ret;
	int iLenPkgBody = pPkgHead->datasize;

	CMemory *p_memory = CMemory::GetInstance();
	char *p_sendbuf = (char *)p_memory->AllocMemory(m_iLenMsgHeader + m_iLenPkgHeader + iLenPkgBody, false); // 准备发送的格式，这里是消息头+包头+包体

	if ((ret = ReadB(pPkgHead->qname, pPkgHead->itemname, p_sendbuf + m_iLenMsgHeader + m_iLenPkgHeader, iLenPkgBody, &timestamp)))
	{
		pPkgHead->error = 0;
		pPkgHead->bodysize = pPkgHead->datasize;
		pPkgHead->timestamp = timestamp;
	}
	else
	{
		pPkgHead->error = GetLastErrorQ();
		pPkgHead->bodysize = 0;
	}

	CLock lock(&pConn->logicPorcMutex); // 凡是和本用户有关的访问都互斥

	// 填充消息头
	memcpy(p_sendbuf, pMsgHeader, m_iLenMsgHeader); // 消息头直接拷贝到这里来
	// 填充包头
	memcpy(p_sendbuf + m_iLenMsgHeader, pPkgHeader, m_iLenPkgHeader); // 包头直接拷贝到这里来

	// 发送数据包
	msgSend(p_sendbuf);

	return true;
}

bool CLogicSocket::HandleWriteB(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char *pPkgHeader, unsigned short iBodyLength)
{
	if (pPkgHeader == NULL)
	{
		return false;
	}

	PPKGHEAD pPkgHead = (PPKGHEAD)pPkgHeader; // 包头
	bool ret;
	if ((ret = WriteB(pPkgHead->qname, pPkgHead->itemname, (char *)pPkgHead + sizeof(PKGHEAD), pPkgHead->datasize)))
	{
		pPkgHead->error = 0;
	}
	else
	{
		pPkgHead->error = GetLastErrorQ();
	}

	pPkgHead->bodysize = 0;

	{
		CLock lock(&pConn->logicPorcMutex); // 凡是和本用户有关的访问都互斥

		int iLenPkgBody = 0;
		CMemory *p_memory = CMemory::GetInstance();
		char *p_sendbuf = (char *)p_memory->AllocMemory(m_iLenMsgHeader + m_iLenPkgHeader + iLenPkgBody, false); // 准备发送的格式，这里是消息头+包头+包体
		// 填充消息头
		memcpy(p_sendbuf, pMsgHeader, m_iLenMsgHeader); // 消息头直接拷贝到这里来
		// 填充包头
		memcpy(p_sendbuf + m_iLenMsgHeader, pPkgHeader, m_iLenPkgHeader); // 包头直接拷贝到这里来

		// 发送数据包
		msgSend(p_sendbuf);
	}

	pPkgHead->bodysize = pPkgHead->datasize; // 为了发布订阅的时候能拿到正确的包体长度，实际没用到

	// 发布订阅
	if (ret && pPkgHead->start == 1)	// 1表示触发发布，0表示不触发发布
	{
		// response_tag 只投递给等待的请求方，不通知普通订阅者
		if (!DeliverResponse(pPkgHead->itemname, (char*)pPkgHead + sizeof(PKGHEAD), pPkgHead->datasize))
		{
			NotifySubscriber(pPkgHead->itemname, (char*)pPkgHead + sizeof(PKGHEAD), pPkgHead->datasize);
		}
	}

	return true;
}

bool CLogicSocket::HandleReadBString(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char *pPkgHeader, unsigned short iBodyLength)
{
	if (pPkgHeader == NULL)
	{
		return false;
	}

	timespec timestamp;

	PPKGHEAD pPkgHead = (PPKGHEAD)pPkgHeader; // 包头
	bool ret;

	CMemory *p_memory = CMemory::GetInstance();
	char *p_sendbuf;
	int strlen = 0; // 接收字符串的实际长度
	if ((ret = ReadB_String2(pPkgHead->qname, pPkgHead->itemname, g_buffer, pPkgHead->datasize, strlen, &timestamp)))
	{
		p_sendbuf = (char *)p_memory->AllocMemory(m_iLenMsgHeader + m_iLenPkgHeader + strlen, false); // 准备发送的格式，这里是消息头+包头+包体

		pPkgHead->error = 0;
		pPkgHead->bodysize = strlen; // 包体长度是实际读取的长度，不是datasize（用户缓冲区的长度）
		pPkgHead->timestamp = timestamp;
	}
	else
	{
		p_sendbuf = (char *)p_memory->AllocMemory(m_iLenMsgHeader + m_iLenPkgHeader, false); // 准备发送的格式，这里是消息头+包头+包体

		pPkgHead->error = GetLastErrorQ();
		pPkgHead->bodysize = 0;
	}

	CLock lock(&pConn->logicPorcMutex); // 凡是和本用户有关的访问都互斥

	// 填充消息头
	memcpy(p_sendbuf, pMsgHeader, m_iLenMsgHeader); // 消息头直接拷贝到这里来
	// 填充包头
	memcpy(p_sendbuf + m_iLenMsgHeader, pPkgHeader, m_iLenPkgHeader); // 包头直接拷贝到这里来
	// 填充包体
	if (ret && strlen > 0) // 如果读取成功，才填充包体
	{
		memcpy(p_sendbuf + m_iLenMsgHeader + m_iLenPkgHeader, g_buffer, strlen); // 跳过消息头，跳过包头，就是包体了
	}
	else // 如果读取失败，包体就不填充了
	{
		// 包体不填充，直接发送空包体
	}

	// 发送数据包
	msgSend(p_sendbuf);

	return true;
}

bool CLogicSocket::HandleWriteBString(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char *pPkgHeader, unsigned short iBodyLength)
{
	if (pPkgHeader == NULL)
	{
		return false;
	}

	PPKGHEAD pPkgHead = (PPKGHEAD)pPkgHeader; // 包头
	bool ret;

	if ((ret = WriteB_String(pPkgHead->qname, pPkgHead->itemname, (char *)pPkgHead + sizeof(PKGHEAD), pPkgHead->datasize)))
	{
		pPkgHead->error = 0;
	}
	else
	{
		pPkgHead->error = GetLastErrorQ();
	}

	pPkgHead->bodysize = 0;

	{
		CLock lock(&pConn->logicPorcMutex); // 凡是和本用户有关的访问都互斥

		int iLenPkgBody = 0;
		CMemory *p_memory = CMemory::GetInstance();
		char *p_sendbuf = (char *)p_memory->AllocMemory(m_iLenMsgHeader + m_iLenPkgHeader + iLenPkgBody, false); // 准备发送的格式，这里是消息头+包头+包体
		// 填充消息头
		memcpy(p_sendbuf, pMsgHeader, m_iLenMsgHeader); // 消息头直接拷贝到这里来
		// 填充包头
		memcpy(p_sendbuf + m_iLenMsgHeader, pPkgHeader, m_iLenPkgHeader); // 包头直接拷贝到这里来

		// 发送数据包
		msgSend(p_sendbuf);
	}

	pPkgHead->bodysize = pPkgHead->datasize; // 为了发布订阅的时候能拿到正确的包体长度，实际没用到

	// 发布订阅
	if (ret && pPkgHead->start == 1)	// 1表示触发发布，0表示不触发发布
	{
		NotifySubscriber(pPkgHead->itemname, (char *)pPkgHead + sizeof(PKGHEAD), pPkgHead->datasize);
	}

	return true;
}

bool CLogicSocket::HandleClearB(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength)
{
	if (pPkgHeader == NULL)
	{
		return false;
	}
	PPKGHEAD pPkgHead = (PPKGHEAD)pPkgHeader; // 包头
	bool ret;
	if ((ret = ClearB(pPkgHead->qname)))
	{
		pPkgHead->error = 0;
	}
	else
	{
		pPkgHead->error = GetLastErrorQ();
	}
	pPkgHead->bodysize = 0;
	CMemory *p_memory = CMemory::GetInstance();
	char *p_sendbuf = (char *)p_memory->AllocMemory(m_iLenMsgHeader + m_iLenPkgHeader, false); // 准备发送的格式，这里是消息头+包头+包体
	// 填充消息头
	memcpy(p_sendbuf, pMsgHeader, m_iLenMsgHeader); // 消息头直接拷贝到这里来
	// 填充包头
	memcpy(p_sendbuf + m_iLenMsgHeader, pPkgHeader, m_iLenPkgHeader); // 包头直接拷贝到这里来
	// 发送数据包
	msgSend(p_sendbuf);
	return true;
}

// 这个函数和windows平台的区别是不返回TAG类型的大小
bool CLogicSocket::HandleSubscribe(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char *pPkgHeader, unsigned short iBodyLength)
{
	if (pPkgHeader == NULL)
	{
		return false;
	}

	PPKGHEAD pPkgHead = (PPKGHEAD)pPkgHeader; // 包头

	CLock lock(&pConn->logicPorcMutex); // 凡是和本用户有关的访问都互斥
	pConn->Attach(pPkgHead->itemname);

	EventNode eventnode;
	eventnode.subscriber = pConn;
	eventnode.eventid = (EVENTID)(pPkgHead->eventid);
	eventnode.eventarg = pPkgHead->eventarg;
	strcpy(eventnode.eventname, pPkgHead->qname); // 用qname字段保存用户定义的事件名！
	m_subscriber.Attach(pPkgHead->itemname, eventnode);

	pPkgHead->error = 0;
	pPkgHead->bodysize = 0;

	int iLenPkgBody = 0;
	CMemory *p_memory = CMemory::GetInstance();
	char *p_sendbuf = (char *)p_memory->AllocMemory(m_iLenMsgHeader + m_iLenPkgHeader + iLenPkgBody, false); // 准备发送的格式，这里是消息头+包头+包体
	// 填充消息头
	memcpy(p_sendbuf, pMsgHeader, m_iLenMsgHeader); // 消息头直接拷贝到这里来
	// 填充包头
	memcpy(p_sendbuf + m_iLenMsgHeader, pPkgHeader, m_iLenPkgHeader); // 包头直接拷贝到这里来

	// 发送数据包
	msgSend(p_sendbuf);

	return true;
}

bool CLogicSocket::HandlePostWait(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char *pPkgHeader = 0, unsigned short iBodyLength = 0)
{
	if (pPkgHeader == NULL)
	{
		return false;
	}

	PPKGHEAD pPkgHead = (PPKGHEAD)pPkgHeader; // 包头

	CLock lock(&pConn->logicPorcMutex); // 凡是和本用户有关的访问都互斥

	if (!pConn->m_listPost.empty())
	{
		pConn->m_bWaitingPost = false;
		char *p_sendbuf = pConn->m_listPost.front();
		pConn->m_listPost.pop_front();

		// 发送数据包
		msgSend(p_sendbuf);
	}
	else
	{
		int dwWaitingTime = pPkgHead->timeout;
		if (dwWaitingTime == -1)
		{
			pConn->m_bWaitingPost = true;
			pConn->m_bWaitingTimeout = false;
		}
		else if (dwWaitingTime == 0)
		{
			pConn->m_bWaitingPost = false;
			pConn->m_bWaitingTimeout = false;

			CMemory *p_memory = CMemory::GetInstance();
			char *p_sendbuf = (char *)p_memory->AllocMemory(m_iLenMsgHeader + m_iLenPkgHeader + 0, false); // 准备发送的格式，这里是消息头+包头
			// b)填充消息头
			memcpy(p_sendbuf, pMsgHeader, m_iLenMsgHeader); // 消息头直接拷贝到这里来
			// c)填充包头
			PPKGHEAD pPkgHead = (PPKGHEAD)(p_sendbuf + m_iLenMsgHeader);
			memset(pPkgHead, 0, sizeof(PKGHEAD));
			pPkgHead->id = POSTWAIT;
			pPkgHead->itemname[0] = '\0';
			pPkgHead->error = ERROR_WAIT_TIMEOUT;
			pPkgHead->bodysize = 0;
			// f)发送数据包
			msgSend(p_sendbuf);
		}
		else
		{
			pConn->m_bWaitingPost = true;
			pConn->m_bWaitingTimeout = true;

			// 启动最小堆定时器
			pConn->StartTimeoutTimer(dwWaitingTime);
		}
	}
	return true;
}

void CLogicSocket::NotifySubscriber(std::string tagName, char *pPkgBody, unsigned short iBodyLength)
{
	std::list<EventNode> subscribers = m_subscriber.GetSubscriber(tagName);

	int usernumber = (int)subscribers.size();

	// mark ?
	if (usernumber > 500)
	{
		ngx_log_stderr(0, "ERROR:可能产生了事件风暴，请检查应用程序");
		exit(1);
	}

	if (usernumber > 0)
	{
		for (auto subscriber : subscribers)
		{
			CMemory *p_memory = CMemory::GetInstance();
			char *p_sendbuf = (char *)p_memory->AllocMemory(m_iLenMsgHeader + m_iLenPkgHeader + iBodyLength, false); // 准备发送的格式，这里是消息头+包头+包体
			// 填充消息头
			LPSTRUC_MSG_HEADER ptmpMsgHeader = (LPSTRUC_MSG_HEADER)p_sendbuf;
			lpngx_connection_t pConn = (lpngx_connection_t)(subscriber.subscriber);
			ptmpMsgHeader->pConn = pConn;
			ptmpMsgHeader->iCurrsequence = ptmpMsgHeader->pConn->iCurrsequence;
			// 填充包头
			PPKGHEAD pPkgHead = (PPKGHEAD)(p_sendbuf + m_iLenMsgHeader); // 包头
			memset(pPkgHead, 0, sizeof(PKGHEAD));
			pPkgHead->id = POST;										 // 发布事件
			strcpy(pPkgHead->itemname, tagName.c_str());				 // 必须的，因为最终发布事件的时候是用的itemname
			pPkgHead->error = 0;										 // 必须设置为0，因为包头是在堆上分配的，所以error值是随机的（而且很有可能是上一次分配的同一块内存的值）
			pPkgHead->bodysize = iBodyLength;
			// 填充包体
			if (pPkgBody != NULL && iBodyLength > 0) // 如果有包体，才拷贝包体
			{
				memcpy(p_sendbuf + m_iLenMsgHeader + m_iLenPkgHeader, pPkgBody, iBodyLength); // 包体直接拷贝到这里来
			}

			// mark 有必要互斥吗？写入发送队列m_MsgSendQueue的时候已经互斥了，这里又不是真正的发送线程
			CLock lock(&pConn->logicPorcMutex); // 凡是和本用户有关的访问都互斥

			switch (subscriber.eventid)
			{
			case EVENTID::DEFAULT:
				// ngx_log_stderr(0, "-------------------CLogicSocket::NotifySubscriber() 事件名=%s, eventarg=%d", pPkgHead->itemname, subscriber.eventarg);
				if (pConn->m_bWaitingTimeout)
				{
					pConn->m_bWaitingTimeout = false;
					pConn->StopTimeoutTimer();
				}

				if (pConn->m_bWaitingPost)
				{
					pConn->m_bWaitingPost = false;
					msgSend(p_sendbuf);
				}
				else
				{
					pConn->m_listPost.push_back(p_sendbuf);
				}
				break;
			case EVENTID::POST_DELAY:
				strcpy(pPkgHead->itemname, subscriber.eventname); // 必须的，因为最终发布事件的时候是用的itemname
				g_tm.add_once(subscriber.eventarg, [](void *arg)
							  {
					char* p_sendbuf = (char*)arg;
					LPSTRUC_MSG_HEADER ptmpMsgHeader = (LPSTRUC_MSG_HEADER)p_sendbuf;
					lpngx_connection_t pconn = ptmpMsgHeader->pConn;
					[[maybe_unused]] PPKGHEAD pPkgHead = (PPKGHEAD)(p_sendbuf + sizeof(STRUC_MSG_HEADER));
					if (pconn->m_bWaitingTimeout)
					{
						pconn->m_bWaitingTimeout = false;
						pconn->StopTimeoutTimer();
					}

					if (pconn->m_bWaitingPost)
					{
						//发送延时事件
						pconn->m_bWaitingPost = false;
						g_socket.msgSend(p_sendbuf);
					}
					else
					{
						pconn->m_listPost.push_back(p_sendbuf);
					} }, p_sendbuf);
				break;
			default:
				ngx_log_stderr(0, "ERROR:unknown eventid");
				p_memory->FreeMemory(p_sendbuf); // 释放内存
				break;
			}
		}
	}
}

void CLogicSocket::NotifyTimerSubscriber(std::string timerName, char *pPkgBody, unsigned short iBodyLength)
{
	const auto &subscribers = m_subscriber.GetSubscriber(timerName);
	if (subscribers.empty())
	{
		return;
	}

	CMemory *p_memory = CMemory::GetInstance();
	for (auto subscriber : subscribers)
	{
		char *p_sendbuf = (char *)p_memory->AllocMemory(m_iLenMsgHeader + m_iLenPkgHeader + iBodyLength, false); // 准备发送的格式，这里是消息头+包头+包体
		// 填写消息头内容
		LPSTRUC_MSG_HEADER ptmpMsgHeader = (LPSTRUC_MSG_HEADER)p_sendbuf;
		lpngx_connection_t pConn = (lpngx_connection_t)(subscriber.subscriber);
		ptmpMsgHeader->pConn = pConn;
		ptmpMsgHeader->iCurrsequence = ptmpMsgHeader->pConn->iCurrsequence;
		// 填充包头
		PPKGHEAD pPkgHead = (PPKGHEAD)(p_sendbuf + m_iLenMsgHeader);
		memset(pPkgHead, 0, sizeof(PKGHEAD));
		pPkgHead->id = POST; // 发布事件
		strncpy(pPkgHead->itemname, timerName.c_str(), sizeof(pPkgHead->itemname) - 1);
		pPkgHead->itemname[sizeof(pPkgHead->itemname) - 1] = '\0'; // 确保字符串零终止
		pPkgHead->bodysize = iBodyLength;
		// 填充包体
		if (pPkgBody != NULL && iBodyLength > 0) // 如果有包体，才拷贝包体
		{
			memcpy(p_sendbuf + m_iLenMsgHeader + m_iLenPkgHeader, pPkgBody, iBodyLength); // 包体直接拷贝到这里来
		}

		CLock lock(&pConn->logicPorcMutex); // 凡是和本用户有关的访问都互斥

		if (pConn->m_bWaitingTimeout)
		{
			pConn->m_bWaitingTimeout = false;
			pConn->StopTimeoutTimer();
		}

		if (pConn->m_bWaitingPost)
		{
			pConn->m_bWaitingPost = false;
			msgSend(p_sendbuf);
		}
		else
		{
			pConn->m_listPost.push_back(p_sendbuf);
		}
	}
}

void CLogicSocket::CancelSubscribe(lpngx_connection_t pConn, const std::list<std::string> &tagList, const std::list<std::string> &plcTagList)
{
	for (auto tag : tagList)
	{
		m_subscriber.Detach(tag, pConn);
	}
	for (auto plctag : plcTagList)
	{
		m_subscriber.DetachPlcIoServer(plctag, pConn);
	}
	pConn->ClearTagList();
}

bool CLogicSocket::HandleReadType(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength)
{
	if (pPkgHeader == NULL)
	{
		return false;
	}

	PPKGHEAD pPkgHead = (PPKGHEAD)pPkgHeader; // 包头
	bool ret;
	int iLenPkgBody = pPkgHead->datasize;

	CMemory* p_memory = CMemory::GetInstance();
	char* p_sendbuf = (char*)p_memory->AllocMemory(m_iLenMsgHeader + m_iLenPkgHeader + iLenPkgBody, false); // 准备发送的格式，这里是消息头+包头+包体

	int typesize = 0;
	if ((ret = ReadType(pPkgHead->qname, pPkgHead->itemname, p_sendbuf + m_iLenMsgHeader + m_iLenPkgHeader, iLenPkgBody, &typesize)))
	{
		pPkgHead->error = 0;
		pPkgHead->bodysize = typesize;
		pPkgHead->recsize = typesize;
	}
	else
	{
		pPkgHead->error = GetLastErrorQ();
		pPkgHead->bodysize = 0;
		pPkgHead->recsize = 0;
	}

	CLock lock(&pConn->logicPorcMutex); // 凡是和本用户有关的访问都互斥

	// 填充消息头
	memcpy(p_sendbuf, pMsgHeader, m_iLenMsgHeader); // 消息头直接拷贝到这里来
	// 填充包头
	memcpy(p_sendbuf + m_iLenMsgHeader, pPkgHeader, m_iLenPkgHeader); // 包头直接拷贝到这里来

	// 发送数据包
	msgSend(p_sendbuf);

	return true;
}

bool CLogicSocket::HandleCreateItem(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char *pPkgHeader, unsigned short iBodyLength)
{
	if (pPkgHeader == NULL)
	{
		return false;
	}

	PPKGHEAD pPkgHead = (PPKGHEAD)pPkgHeader; // 包头
	bool ret;
	if ((ret = CreateItem(pPkgHead->qname, pPkgHead->itemname, pPkgHead->recsize, (char *)pPkgHead + sizeof(PKGHEAD), pPkgHead->bodysize)))
	{
		pPkgHead->error = 0;
	}
	else
	{
		pPkgHead->error = GetLastErrorQ();
	}

	pPkgHead->bodysize = 0;

	CLock lock(&pConn->logicPorcMutex); // 凡是和本用户有关的访问都互斥

	int iLenPkgBody = 0;
	CMemory *p_memory = CMemory::GetInstance();
	char *p_sendbuf = (char *)p_memory->AllocMemory(m_iLenMsgHeader + m_iLenPkgHeader + iLenPkgBody, false); // 准备发送的格式，这里是消息头+包头+包体
	// b)填充消息头
	memcpy(p_sendbuf, pMsgHeader, m_iLenMsgHeader); // 消息头直接拷贝到这里来
	// c)填充包头
	memcpy(p_sendbuf + m_iLenMsgHeader, pPkgHeader, m_iLenPkgHeader); // 包头直接拷贝到这里来

	// f)发送数据包
	msgSend(p_sendbuf);

	return true;
}

bool CLogicSocket::HandleDeleteItem(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength)
{
	if (pPkgHeader == NULL)
	{
		return false;
	}
	PPKGHEAD pPkgHead = (PPKGHEAD)pPkgHeader; // 包头
	bool ret;
	if ((ret = DeleteItem(pPkgHead->qname, pPkgHead->itemname)))
	{
		pPkgHead->error = 0;
	}
	else
	{
		pPkgHead->error = GetLastErrorQ();
	}
	pPkgHead->bodysize = 0;
	CLock lock(&pConn->logicPorcMutex); // 凡是和本用户有关的访问都互斥
	int iLenPkgBody = 0;
	CMemory *p_memory = CMemory::GetInstance();
	char *p_sendbuf = (char *)p_memory->AllocMemory(m_iLenMsgHeader + m_iLenPkgHeader + iLenPkgBody, false); // 准备发送的格式，这里是消息头+包头+包体
	// b)填充消息头
	memcpy(p_sendbuf, pMsgHeader, m_iLenMsgHeader); // 消息头直接拷贝到这里来
	// c)填充包头
	memcpy(p_sendbuf + m_iLenMsgHeader, pPkgHeader, m_iLenPkgHeader); // 包头直接拷贝到这里来
	// f)发送数据包
	msgSend(p_sendbuf);
	return true;
}

bool CLogicSocket::HandleRegisterPlcServer(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char *pPkgHeader, unsigned short iBodyLength)
{
	if (pPkgHeader == NULL)
	{
		return false;
	}

	PPKGHEAD pPkgHead = (PPKGHEAD)pPkgHeader; // 包头

	CLock lock(&pConn->logicPorcMutex); // 凡是和本用户有关的访问都互斥
	pConn->AttachPlcTag(pPkgHead->itemname);

	m_subscriber.AttachPlcIoServer(pPkgHead->itemname, pConn);

	pPkgHead->error = 0;
	pPkgHead->bodysize = 0;

	int iLenPkgBody = 0;
	CMemory *p_memory = CMemory::GetInstance();
	char *p_sendbuf = (char *)p_memory->AllocMemory(m_iLenMsgHeader + m_iLenPkgHeader + iLenPkgBody, false); // 准备发送的格式，这里是消息头+包头+包体
	// 填充消息头
	memcpy(p_sendbuf, pMsgHeader, m_iLenMsgHeader); // 消息头直接拷贝到这里来
	// 填充包头
	memcpy(p_sendbuf + m_iLenMsgHeader, pPkgHeader, m_iLenPkgHeader); // 包头直接拷贝到这里来

	// 发送数据包
	msgSend(p_sendbuf);

	return true;
}

bool CLogicSocket::HandleWriteBPlc(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char *pPkgHeader, unsigned short iBodyLength)
{
	if (pPkgHeader == NULL)
	{
		return false;
	}

	PPKGHEAD pPkgHead = (PPKGHEAD)pPkgHeader; // 包头
	// bool ret;
	// if (ret = WriteB(pPkgHead->qname, pPkgHead->itemname, (char*)pPkgHead + sizeof(PKGHEAD), pPkgHead->datasize))
	// {
	// 	pPkgHead->error = 0;
	// }
	// else
	// {
	// 	pPkgHead->error = GetLastErrorQ();
	// }
	pPkgHead->error = 0; // 先假设写PLC成功了，等发布订阅的时候再根据实际情况修改这个值

	pPkgHead->bodysize = 0;

	{
		CLock lock(&pConn->logicPorcMutex); // 凡是和本用户有关的访问都互斥

		int iLenPkgBody = 0;
		CMemory *p_memory = CMemory::GetInstance();
		char *p_sendbuf = (char *)p_memory->AllocMemory(m_iLenMsgHeader + m_iLenPkgHeader + iLenPkgBody, false); // 准备发送的格式，这里是消息头+包头+包体
		// 填充消息头
		memcpy(p_sendbuf, pMsgHeader, m_iLenMsgHeader); // 消息头直接拷贝到这里来
		// 填充包头
		memcpy(p_sendbuf + m_iLenMsgHeader, pPkgHeader, m_iLenPkgHeader); // 包头直接拷贝到这里来

		// 发送数据包
		msgSend(p_sendbuf);
	}

	pPkgHead->bodysize = pPkgHead->datasize; // 为了发布订阅的时候能拿到正确的包体长度，实际没用到

	// 发布订阅
	NotifyPlcIoSever(pPkgHead->itemname, (char *)pPkgHead + sizeof(PKGHEAD), pPkgHead->datasize);

	return true;
}

bool CLogicSocket::HandleWriteBStringPlc(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char *pPkgHeader, unsigned short iBodyLength)
{
	if (pPkgHeader == NULL)
	{
		return false;
	}

	PPKGHEAD pPkgHead = (PPKGHEAD)pPkgHeader; // 包头
	// bool ret;

	// if (ret = WriteB_String(pPkgHead->qname, pPkgHead->itemname, (char *)pPkgHead + sizeof(PKGHEAD), pPkgHead->datasize))
	// {
	// 	pPkgHead->error = 0;
	// }
	// else
	// {
	// 	pPkgHead->error = GetLastErrorQ();
	// }
	pPkgHead->error = 0; // 先假设写PLC成功了，等发布订阅的时候再根据实际情况修改这个值

	pPkgHead->bodysize = 0;

	{
		CLock lock(&pConn->logicPorcMutex); // 凡是和本用户有关的访问都互斥

		int iLenPkgBody = 0;
		CMemory *p_memory = CMemory::GetInstance();
		char *p_sendbuf = (char *)p_memory->AllocMemory(m_iLenMsgHeader + m_iLenPkgHeader + iLenPkgBody, false); // 准备发送的格式，这里是消息头+包头+包体
		// 填充消息头
		memcpy(p_sendbuf, pMsgHeader, m_iLenMsgHeader); // 消息头直接拷贝到这里来
		// 填充包头
		memcpy(p_sendbuf + m_iLenMsgHeader, pPkgHeader, m_iLenPkgHeader); // 包头直接拷贝到这里来

		// 发送数据包
		msgSend(p_sendbuf);
	}

	pPkgHead->bodysize = pPkgHead->datasize; // 为了发布订阅的时候能拿到正确的包体长度，实际没用到

	// 发布订阅
	NotifyPlcIoSever(pPkgHead->itemname, (char *)pPkgHead + sizeof(PKGHEAD), pPkgHead->datasize);

	return true;
}

void CLogicSocket::NotifyPlcIoSever(std::string tagName, char *pPkgBody, unsigned short iBodyLength)
{
	void *subscriber = m_subscriber.GetPlcIoServer(tagName);

	if (subscriber != nullptr)
	{
			CMemory *p_memory = CMemory::GetInstance();
			char *p_sendbuf = (char *)p_memory->AllocMemory(m_iLenMsgHeader + m_iLenPkgHeader + iBodyLength, false); // 准备发送的格式，这里是消息头+包头+包体
			// 填充消息头
			LPSTRUC_MSG_HEADER ptmpMsgHeader = (LPSTRUC_MSG_HEADER)p_sendbuf;
			lpngx_connection_t pConn = (lpngx_connection_t)(subscriber);
			ptmpMsgHeader->pConn = pConn;
			ptmpMsgHeader->iCurrsequence = ptmpMsgHeader->pConn->iCurrsequence;
			// 填充包头
			PPKGHEAD pPkgHead = (PPKGHEAD)(p_sendbuf + m_iLenMsgHeader); // 包头
			memset(pPkgHead, 0, sizeof(PKGHEAD));
			pPkgHead->id = POST;										 // 发布事件
			strcpy(pPkgHead->itemname, tagName.c_str());				 // 必须的，因为最终发布事件的时候是用的itemname
			pPkgHead->error = 0;										 // 必须设置为0，因为包头是在堆上分配的，所以error值是随机的（而且很有可能是上一次分配的同一块内存的值）
			pPkgHead->bodysize = iBodyLength;
			// 填充包体
			if (pPkgBody != NULL && iBodyLength > 0) // 如果有包体，才拷贝包体
			{
				memcpy(p_sendbuf + m_iLenMsgHeader + m_iLenPkgHeader, pPkgBody, iBodyLength); // 包体直接拷贝到这里来
			}

			// mark 有必要互斥吗？写入发送队列m_MsgSendQueue的时候已经互斥了，这里又不是真正的发送线程
			CLock lock(&pConn->logicPorcMutex); // 凡是和本用户有关的访问都互斥

			if (pConn->m_bWaitingTimeout)
			{
				pConn->m_bWaitingTimeout = false;
				pConn->StopTimeoutTimer();
			}

			if (pConn->m_bWaitingPost)
			{
				pConn->m_bWaitingPost = false;
				msgSend(p_sendbuf);
			}
			else
			{
				pConn->m_listPost.push_back(p_sendbuf);
			}
	}
	else
	{
		ngx_log_stderr(0, "ERROR:没有PLCIO服务器订阅这个TAG，请检查应用程序,%s这个TAG没有PLCIO服务器订阅了", tagName.c_str());
	}
}

bool CLogicSocket::HandleReadBoardInfo(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength)
{
	if (pPkgHeader == NULL)
	{
		return false;
	}
	PPKGHEAD pPkgHead = (PPKGHEAD)pPkgHeader; // 包头
	bool ret;
	int iLenPkgBody = pPkgHead->datasize;
	CMemory* p_memory = CMemory::GetInstance();
	char* p_sendbuf = (char*)p_memory->AllocMemory(m_iLenMsgHeader + m_iLenPkgHeader + iLenPkgBody, false); // 准备发送的格式，这里是消息头+包头+包体
	if (pPkgHead->datasize == sizeof(BOARD_INFO))
	{
		if ((ret = ReadBoardInfo(pPkgHead->qname, (BOARD_INFO*)(p_sendbuf + m_iLenMsgHeader + m_iLenPkgHeader))))
		{
			pPkgHead->error = 0;
			pPkgHead->bodysize = iLenPkgBody;
		}
		else
		{
			pPkgHead->error = GetLastErrorQ();
			pPkgHead->bodysize = 0;
		}
	}
	else
	{
		pPkgHead->error = ERROR_RECORDSIZE;
		pPkgHead->bodysize = 0;
	}
	CLock lock(&pConn->logicPorcMutex); // 凡是和本用户有关的访问都互斥
	// 填充消息头
	memcpy(p_sendbuf, pMsgHeader, m_iLenMsgHeader); // 消息头直接拷贝到这里来
	// 填充包头
	memcpy(p_sendbuf + m_iLenMsgHeader, pPkgHeader, m_iLenPkgHeader); // 包头直接拷贝到这里来
	// 发送数据包
	msgSend(p_sendbuf);

	return true;
}

bool CLogicSocket::HandleCreateQueue(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength)
{
	if (pPkgHeader == NULL)
	{
		return false;
	}

	PPKGHEAD pPkgHead = (PPKGHEAD)pPkgHeader; // 包头
	bool ret;

	//extern "C" bool CreateQ(const char* lpFileName,
	//	int recordSize,
	//	int recordNum,
	//	int dateType,
	//	int operateMode,
	//	void* pType,
	//	int typeSize)
	if ((ret = CreateQ(pPkgHead->qname, pPkgHead->recsize, pPkgHead->count, 0, pPkgHead->start, (char*)pPkgHead + sizeof(PKGHEAD), pPkgHead->bodysize)))
	{
		pPkgHead->error = 0;
	}
	else
	{
		pPkgHead->error = GetLastErrorQ();
	}

	pPkgHead->bodysize = 0;

	CLock lock(&pConn->logicPorcMutex); // 凡是和本用户有关的访问都互斥

	int iLenPkgBody = 0;
	CMemory* p_memory = CMemory::GetInstance();
	char* p_sendbuf = (char*)p_memory->AllocMemory(m_iLenMsgHeader + m_iLenPkgHeader + iLenPkgBody, false); // 准备发送的格式，这里是消息头+包头+包体
	// b)填充消息头
	memcpy(p_sendbuf, pMsgHeader, m_iLenMsgHeader); // 消息头直接拷贝到这里来
	// c)填充包头
	memcpy(p_sendbuf + m_iLenMsgHeader, pPkgHeader, m_iLenPkgHeader); // 包头直接拷贝到这里来

	// f)发送数据包
	msgSend(p_sendbuf);

	return true;
}

// 请求包：itemname=request_tag, qname=response_tag, datasize=请求大小, recsize=响应大小, timeout=超时毫秒
bool CLogicSocket::HandleGetResponse(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength)
{
	if (pPkgHeader == NULL)
	{
		return false;
	}

	PPKGHEAD pPkgHead = (PPKGHEAD)pPkgHeader;
	pPkgHead->itemname[sizeof(pPkgHead->itemname) - 1] = '\0';
	pPkgHead->qname[sizeof(pPkgHead->qname) - 1] = '\0';

	auto req = std::make_shared<PendingRequest>();
	req->pConn = pConn;
	req->iCurrsequence = pMsgHeader->iCurrsequence;
	req->requestTag = pPkgHead->itemname;
	req->responseTag = pPkgHead->qname;
	req->responseSize = pPkgHead->recsize;

	int requestSize = pPkgHead->datasize;
	int timeout = pPkgHead->timeout;
	unsigned int error = 0;

	if (req->requestTag.empty() || req->responseTag.empty() || req->requestTag == req->responseTag ||
		timeout <= 0 || requestSize <= 0 || requestSize != iBodyLength ||
		req->responseSize <= 0 || req->responseSize > MAXMSGLEN)
	{
		error = ERROR_INVALID_PARAMETER;
	}
	// 借助 ReadB 校验两个 tag 存在且大小一致
	else if (!ReadB("BOARD", req->requestTag.c_str(), g_buffer, requestSize) ||
			 !ReadB("BOARD", req->responseTag.c_str(), g_buffer, req->responseSize))
	{
		error = GetLastErrorQ();
	}

	if (error != 0)
	{
		SendGetResponseReply(req, error, nullptr, 0);
		return true;
	}

	char* pData = (char*)pPkgHead + sizeof(PKGHEAD);
	req->requestData.assign(pData, pData + requestSize);

	bool startNow = false;
	{
		std::lock_guard<std::mutex> lock(m_reqMutex);

		auto owner = m_mapResponseOwner.find(req->responseTag);
		if (owner != m_mapResponseOwner.end() && owner->second != req->requestTag)
		{
			error = ERROR_INVALID_PARAMETER;	// response_tag 不得被多个 request_tag 共用
		}
		else
		{
			RequestChannel& channel = m_mapReqChannel[req->requestTag];
			if (channel.active && channel.waiting.size() >= REQUEST_QUEUE_MAX)
			{
				error = ERROR_REQUEST_QUEUE_FULL;
			}
			else
			{
				m_mapResponseOwner[req->responseTag] = req->requestTag;

				std::weak_ptr<PendingRequest> weakReq = req;
				req->timerId = g_tm.add_once(timeout, [this, weakReq](void*) { OnRequestTimeout(weakReq); }, nullptr);

				if (channel.active)
				{
					channel.waiting.push_back(req);
				}
				else
				{
					channel.active = req;
					startNow = true;
				}
			}
		}
	}

	if (error != 0)
	{
		ngx_log_error_core(NGX_LOG_WARN, 0, "getresponse被拒绝: request=%s, response=%s, error=%d",
			req->requestTag.c_str(), req->responseTag.c_str(), error);
		SendGetResponseReply(req, error, nullptr, 0);
		return true;
	}

	if (startNow)
	{
		StartRequest(req);
	}

	return true;
}

// 写入请求并通知响应方；写入失败则结束该请求并继续处理下一个
void CLogicSocket::StartRequest(PendingRequestPtr req)
{
	while (req)
	{
		if (WriteB("BOARD", req->requestTag.c_str(), req->requestData.data(), (int)req->requestData.size()))
		{
			NotifySubscriber(req->requestTag, req->requestData.data(), (unsigned short)req->requestData.size());
			return;
		}

		unsigned int error = GetLastErrorQ();
		PendingRequestPtr next;
		{
			std::lock_guard<std::mutex> lock(m_reqMutex);
			if (!DetachRequestLocked(req, next))
			{
				return;
			}
		}
		SendGetResponseReply(req, error, nullptr, 0);
		req = next;
	}
}

// 返回 false 表示 responseTag 不是 response_tag，调用者按普通 writeb 处理
bool CLogicSocket::DeliverResponse(const char* responseTag, char* pData, int iDataLen)
{
	PendingRequestPtr req;
	PendingRequestPtr next;
	{
		std::lock_guard<std::mutex> lock(m_reqMutex);

		auto owner = m_mapResponseOwner.find(responseTag);
		if (owner == m_mapResponseOwner.end())
		{
			return false;
		}

		auto it = m_mapReqChannel.find(owner->second);
		if (it != m_mapReqChannel.end() && it->second.active && it->second.active->responseTag == responseTag)
		{
			req = it->second.active;
			DetachRequestLocked(req, next);
		}
	}

	if (!req)
	{
		ngx_log_error_core(NGX_LOG_WARN, 0, "丢弃无人等待的响应: response=%s", responseTag);
		return true;
	}

	if (iDataLen != req->responseSize)
	{
		SendGetResponseReply(req, ERROR_RECORDSIZE, nullptr, 0);
	}
	else
	{
		SendGetResponseReply(req, 0, pData, iDataLen);
	}

	if (next)
	{
		StartRequest(next);
	}
	return true;
}

void CLogicSocket::OnRequestTimeout(const std::weak_ptr<PendingRequest>& weakReq)
{
	PendingRequestPtr req = weakReq.lock();
	if (!req)
	{
		return;
	}

	PendingRequestPtr next;
	{
		std::lock_guard<std::mutex> lock(m_reqMutex);
		req->timerId = -1;	// 定时器已触发，无需再取消
		if (!DetachRequestLocked(req, next))
		{
			return;
		}
	}

	ngx_log_error_core(NGX_LOG_WARN, 0, "getresponse超时: request=%s, response=%s",
		req->requestTag.c_str(), req->responseTag.c_str());
	SendGetResponseReply(req, ERROR_RESPONSE_TIMEOUT, nullptr, 0);

	if (next)
	{
		StartRequest(next);
	}
}

// 调用前须持有 m_reqMutex；req 为 active 时把排队中的下一个提升为 active 并通过 next 返回
bool CLogicSocket::DetachRequestLocked(const PendingRequestPtr& req, PendingRequestPtr& next)
{
	auto it = m_mapReqChannel.find(req->requestTag);
	if (it == m_mapReqChannel.end())
	{
		return false;
	}

	RequestChannel& channel = it->second;
	if (channel.active == req)
	{
		channel.active.reset();
		if (!channel.waiting.empty())
		{
			channel.active = channel.waiting.front();
			channel.waiting.pop_front();
			next = channel.active;
		}
	}
	else
	{
		auto w = std::find(channel.waiting.begin(), channel.waiting.end(), req);
		if (w == channel.waiting.end())
		{
			return false;
		}
		channel.waiting.erase(w);
	}

	if (req->timerId >= 0)
	{
		g_tm.cancel(req->timerId);
		req->timerId = -1;
	}

	if (!channel.active && channel.waiting.empty())
	{
		m_mapReqChannel.erase(it);
	}
	return true;
}

void CLogicSocket::SendGetResponseReply(const PendingRequestPtr& req, unsigned int error, const char* pBody, int iBodyLen)
{
	CMemory* p_memory = CMemory::GetInstance();
	char* p_sendbuf = (char*)p_memory->AllocMemory(m_iLenMsgHeader + m_iLenPkgHeader + iBodyLen, false);

	LPSTRUC_MSG_HEADER ptmpMsgHeader = (LPSTRUC_MSG_HEADER)p_sendbuf;
	ptmpMsgHeader->pConn = req->pConn;
	ptmpMsgHeader->iCurrsequence = req->iCurrsequence;	// 连接已断开时由发送线程丢弃

	PPKGHEAD pPkgHead = (PPKGHEAD)(p_sendbuf + m_iLenMsgHeader);
	memset(pPkgHead, 0, sizeof(PKGHEAD));
	pPkgHead->id = GETRESPONSE;
	strncpy(pPkgHead->itemname, req->requestTag.c_str(), sizeof(pPkgHead->itemname) - 1);
	strncpy(pPkgHead->qname, req->responseTag.c_str(), sizeof(pPkgHead->qname) - 1);
	pPkgHead->error = error;
	pPkgHead->bodysize = iBodyLen;
	if (pBody != nullptr && iBodyLen > 0)
	{
		memcpy(p_sendbuf + m_iLenMsgHeader + m_iLenPkgHeader, pBody, iBodyLen);
	}

	msgSend(p_sendbuf);
}

// 连接断开时释放其占用的请求，并继续处理排队的下一个
void CLogicSocket::CancelRequest(lpngx_connection_t pConn)
{
	std::vector<PendingRequestPtr> nextList;
	{
		std::lock_guard<std::mutex> lock(m_reqMutex);

		std::vector<PendingRequestPtr> owned;
		for (auto& kv : m_mapReqChannel)
		{
			if (kv.second.active && kv.second.active->pConn == pConn)
			{
				owned.push_back(kv.second.active);
			}
			for (auto& w : kv.second.waiting)
			{
				if (w->pConn == pConn)
				{
					owned.push_back(w);
				}
			}
		}

		for (auto& req : owned)
		{
			PendingRequestPtr next;
			if (DetachRequestLocked(req, next) && next)
			{
				nextList.push_back(next);
			}
		}
	}

	for (auto& next : nextList)
	{
		StartRequest(next);
	}
}