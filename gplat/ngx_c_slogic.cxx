
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

#include "ngx_c_conf.h"
#include "ngx_macro.h"
#include "ngx_global.h"
#include "ngx_func.h"
#include "ngx_c_memory.h"
#include "ngx_c_crc32.h"
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
	void *pPkgBody;												 // 指向包体的指针

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
	if (ret = ReadQ(pPkgHead->qname, p_sendbuf + m_iLenMsgHeader + m_iLenPkgHeader, iLenPkgBody, ip))
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
	char *data = (char *)pPkgHead + sizeof(PKGHEAD);
	if (ret = WriteQ(pPkgHead->qname, (char *)pPkgHead + sizeof(PKGHEAD), pPkgHead->datasize))
	{
		pPkgHead->error = 0;
	}
	else
	{
		pPkgHead->error = GetLastErrorQ();
	}

	pPkgHead->bodysize = 0;

	CLock lock(&pConn->logicPorcMutex); // 凡是和本用户有关的访问都互斥

	CMemory *p_memory = CMemory::GetInstance();
	char *p_sendbuf = (char *)p_memory->AllocMemory(m_iLenMsgHeader + m_iLenPkgHeader, false); // 准备发送的格式，这里是消息头+包头+包体
	// 填充消息头
	memcpy(p_sendbuf, pMsgHeader, m_iLenMsgHeader); // 消息头直接拷贝到这里来
	// 填充包头
	memcpy(p_sendbuf + m_iLenMsgHeader, pPkgHeader, m_iLenPkgHeader); // 包头直接拷贝到这里来

	// 发送数据包
	msgSend(p_sendbuf);

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

	if (ret = ClearQ(pPkgHead->qname))
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

	if (ret = ReadB(pPkgHead->qname, pPkgHead->itemname, p_sendbuf + m_iLenMsgHeader + m_iLenPkgHeader, iLenPkgBody, &timestamp))
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
	if (ret = WriteB(pPkgHead->qname, pPkgHead->itemname, (char *)pPkgHead + sizeof(PKGHEAD), pPkgHead->datasize))
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
	// 填充消息头
	memcpy(p_sendbuf, pMsgHeader, m_iLenMsgHeader); // 消息头直接拷贝到这里来
	// 填充包头
	memcpy(p_sendbuf + m_iLenMsgHeader, pPkgHeader, m_iLenPkgHeader); // 包头直接拷贝到这里来

	// 发送数据包
	msgSend(p_sendbuf);

	pPkgHead->bodysize = pPkgHead->datasize; // 为了发布订阅的时候能拿到正确的包体长度，实际没用到

	// 发布订阅
	if (ret && pPkgHead->start == 1)	// 1表示触发发布，0表示不触发发布
	{
		NotifySubscriber(pPkgHead->itemname, (char*)pPkgHead + sizeof(PKGHEAD), pPkgHead->datasize);
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
	if (ret = ReadB_String2(pPkgHead->qname, pPkgHead->itemname, g_buffer, pPkgHead->datasize, strlen, &timestamp))
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

	if (ret = WriteB_String(pPkgHead->qname, pPkgHead->itemname, (char *)pPkgHead + sizeof(PKGHEAD), pPkgHead->datasize))
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
	// 填充消息头
	memcpy(p_sendbuf, pMsgHeader, m_iLenMsgHeader); // 消息头直接拷贝到这里来
	// 填充包头
	memcpy(p_sendbuf + m_iLenMsgHeader, pPkgHeader, m_iLenPkgHeader); // 包头直接拷贝到这里来

	// 发送数据包
	msgSend(p_sendbuf);

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
	if (ret = ClearB(pPkgHead->qname))
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
			pPkgHead->id = POSTWAIT;
			pPkgHead->itemname[0] = '\0';
			pPkgHead->error = ETIMEDOUT;
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
					PPKGHEAD pPkgHead = (PPKGHEAD)(p_sendbuf + sizeof(STRUC_MSG_HEADER));
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
		char *p_sendbuf = (char *)p_memory->AllocMemory(m_iLenMsgHeader + m_iLenPkgHeader, false); // 准备发送的格式，这里是消息头+包头
		// 填写消息头内容
		LPSTRUC_MSG_HEADER ptmpMsgHeader = (LPSTRUC_MSG_HEADER)p_sendbuf;
		lpngx_connection_t pConn = (lpngx_connection_t)(subscriber.subscriber);
		ptmpMsgHeader->pConn = pConn;
		ptmpMsgHeader->iCurrsequence = ptmpMsgHeader->pConn->iCurrsequence;
		// 填充包头
		PPKGHEAD pPkgHead = (PPKGHEAD)(p_sendbuf + m_iLenMsgHeader);
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
	if (ret = ReadType(pPkgHead->qname, pPkgHead->itemname, p_sendbuf + m_iLenMsgHeader + m_iLenPkgHeader, iLenPkgBody, &typesize))
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
	if (ret = CreateItem(pPkgHead->qname, pPkgHead->itemname, pPkgHead->recsize, (char *)pPkgHead + sizeof(PKGHEAD), pPkgHead->bodysize))
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
	if (ret = DeleteItem(pPkgHead->qname, pPkgHead->itemname))
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

	pPkgHead->bodysize = pPkgHead->datasize; // 为了发布订阅的时候能拿到正确的包体长度，实际没用到

	// 发布订阅
	NotifyPlcIoSever(pPkgHead->itemname, (char *)pPkgHead + sizeof(PKGHEAD), pPkgHead->datasize);

	return true;
}

void CLogicSocket::NotifyPlcIoSever(std::string tagName, char *pPkgBody, unsigned short iBodyLength)
{
	std::list<void *> subscribers = m_subscriber.GetPlcIoServer(tagName);

	int usernumber = (int)subscribers.size();

	if (usernumber > 1)
	{
		ngx_log_stderr(0, "ERROR:一个TAG只能对应一个PLCIO服务器，请检查应用程序,%s这个TAG有%d个PLCIO服务器订阅了", tagName.c_str(), usernumber);
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
			lpngx_connection_t pConn = (lpngx_connection_t)(subscriber);
			ptmpMsgHeader->pConn = pConn;
			ptmpMsgHeader->iCurrsequence = ptmpMsgHeader->pConn->iCurrsequence;
			// 填充包头
			PPKGHEAD pPkgHead = (PPKGHEAD)(p_sendbuf + m_iLenMsgHeader); // 包头
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
		if (ret = ReadBoardInfo(pPkgHead->qname, (BOARD_INFO*)(p_sendbuf + m_iLenMsgHeader + m_iLenPkgHeader)))
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