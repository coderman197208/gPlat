
// 网络以及逻辑处理有关

#include <cstring>
#include <algorithm>
#include <iterator>
#include <string_view>

#include "ngx_c_conf.h"
#include "ngx_macro.h"
#include "ngx_global.h"
#include "ngx_func.h"
#include "ngx_c_memory.h"
#include "ngx_c_slogic.h"
#include "ngx_logiccomm.h"
#include "ngx_c_lockmutex.h"

#include "../include/higplat.h"

namespace
{
	constexpr int kMsgHeaderLen = sizeof(STRUC_MSG_HEADER);
	constexpr int kPkgHeaderLen = sizeof(PKGHEAD);

	// 线程私有的临时缓冲区（读字符串、getresponse 校验 tag）
	thread_local char g_buffer[MAXMSGLEN] = {0};

	// 定时器是虚拟 tag（不在 Board 中），由 ngx_process_cycle.cxx 中的定时器发布
	constexpr std::string_view kTimerTags[] = {"timer_500ms", "timer_1s", "timer_2s", "timer_3s", "timer_5s"};

	bool IsTimerTag(const char* tagName)
	{
		return std::find(std::begin(kTimerTags), std::end(kTimerTags), tagName) != std::end(kTimerTags);
	}

	// 发送缓冲区布局：消息头 + 包头 + 包体
	char* AllocSendBuf(int iBodyLen)
	{
		return static_cast<char*>(CMemory::GetInstance()->AllocMemory(kMsgHeaderLen + kPkgHeaderLen + iBodyLen, false));
	}

	PPKGHEAD PkgHeadOf(char* p_sendbuf)
	{
		return reinterpret_cast<PPKGHEAD>(p_sendbuf + kMsgHeaderLen);
	}

	char* BodyOf(char* p_sendbuf)
	{
		return p_sendbuf + kMsgHeaderLen + kPkgHeaderLen;
	}

	// 请求包中紧跟包头的数据
	char* PayloadOf(PPKGHEAD pPkgHead)
	{
		return reinterpret_cast<char*>(pPkgHead) + sizeof(PKGHEAD);
	}

	template <size_t N>
	void CopyName(char (&dst)[N], const char* src)
	{
		strncpy(dst, src, N - 1);
		dst[N - 1] = '\0';
	}

	unsigned int ResultCode(bool ok)
	{
		return ok ? 0 : GetLastErrorQ();
	}

	// 客户端给出的应答缓冲区大小：超出协议上限时拒绝，避免按非法长度分配内存
	bool IsValidBodySize(int size)
	{
		return size >= 0 && size <= MAXMSGLEN;
	}

	// 声明的数据长度不得超过实际收到的包体，否则会读越界；不符时置 ERROR_RECORDSIZE
	bool PayloadFits(PPKGHEAD pPkgHead, unsigned short iBodyLength)
	{
		if (pPkgHead->datasize >= 0 && pPkgHead->datasize <= iBodyLength)
		{
			return true;
		}
		pPkgHead->error = ERROR_RECORDSIZE;
		return false;
	}

	// 构造一个主动发往 pConn 的包：消息头 + 清零后只填 id/bodysize 的包头 + 包体，其余包头字段由调用者填写
	char* BuildPacket(lpngx_connection_t pConn, uint64_t iCurrsequence, int id, const char* pBody, int iBodyLen)
	{
		char* p_sendbuf = AllocSendBuf(iBodyLen);

		auto* pMsgHeader = reinterpret_cast<LPSTRUC_MSG_HEADER>(p_sendbuf);
		pMsgHeader->pConn = pConn;
		pMsgHeader->iCurrsequence = iCurrsequence; // 连接已断开时由发送线程丢弃

		PPKGHEAD pPkgHead = PkgHeadOf(p_sendbuf);
		memset(pPkgHead, 0, sizeof(PKGHEAD));
		pPkgHead->id = id;
		pPkgHead->bodysize = iBodyLen;
		if (pBody != nullptr && iBodyLen > 0)
		{
			memcpy(BodyOf(p_sendbuf), pBody, iBodyLen);
		}
		return p_sendbuf;
	}

	// 发布事件包，itemname 为事件名（订阅端据此识别事件）
	char* BuildPostPacket(lpngx_connection_t pConn, const char* name, const char* pBody, unsigned short iBodyLen)
	{
		char* p_sendbuf = BuildPacket(pConn, pConn->iCurrsequence, POST, pBody, iBodyLen);
		CopyName(PkgHeadOf(p_sendbuf)->itemname, name);
		return p_sendbuf;
	}
}

// 成员函数指针，顺序必须和 msg.h 中的 MSGID 对应
using Handler = bool (CLogicSocket::*)(lpngx_connection_t pConn,		 // 连接池中连接的指针
									   LPSTRUC_MSG_HEADER pMsgHeader, // 消息头指针
									   char *pPkgHeader,				 // 包头指针（包体紧随其后）
									   unsigned short iBodyLength);	 // 包体长度

static constexpr Handler statusHandler[] =
	{
		// 数组前5个元素，保留，以备将来增加一些基本服务器功能
		nullptr, // 【0】：下标从0开始
		nullptr, // 【1】
		nullptr, // 【2】
		nullptr, // 【3】
		nullptr, // 【4】

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
		&CLogicSocket::HandlePeekQ,			  // PEEKQ
		&CLogicSocket::HandleWriteQ,		  // WRITEQ
		&CLogicSocket::HandleReadB,			  // READB
		&CLogicSocket::HandleReadBString,	  // READBSTRING
		&CLogicSocket::HandleWriteB,		  // WRITEB
		&CLogicSocket::noop,				  // QDATA
		&CLogicSocket::HandleReadHead,		  // READHEAD
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
		&CLogicSocket::HandleListQ,         // LISTQ
		&CLogicSocket::HandleListTags,      // LISTTAGS
};

static_assert(std::size(statusHandler) == LISTTAGS + 1, "statusHandler[] must have one entry per MSGID");

// 初始化函数【fork()子进程之前执行】
bool CLogicSocket::Initialize()
{
	return CSocekt::Initialize();
}

// 处理收到的数据包，由线程池来调用本函数
// pMsgBuf：消息头 + 包头 + 包体
void CLogicSocket::threadRecvProcFunc(char *pMsgBuf)
{
	auto* pMsgHeader = reinterpret_cast<LPSTRUC_MSG_HEADER>(pMsgBuf);
	PPKGHEAD pPkgHead = PkgHeadOf(pMsgBuf);
	lpngx_connection_t pConn = pMsgHeader->pConn;

	// 从收到包到线程池处理期间客户端断开、连接已被其他 socket 复用，这种包直接丢弃
	if (pConn->iCurrsequence != pMsgHeader->iCurrsequence)
	{
		return;
	}

	// 防止客户端发送不在处理范围内的消息码
	const int msgCode = pPkgHead->id;
	if (msgCode < 0 || static_cast<size_t>(msgCode) >= std::size(statusHandler))
	{
		ngx_log_stderr(0, "CLogicSocket::threadRecvProcFunc()中imsgCode=%d消息码不对!", msgCode);
		return;
	}

	const Handler handler = statusHandler[msgCode];
	if (handler == nullptr)
	{
		ngx_log_stderr(0, "CLogicSocket::threadRecvProcFunc()中imsgCode=%d消息码找不到对应的处理函数!", msgCode);
		return;
	}

	// 名字字段来自客户端，统一保证零终止
	pPkgHead->qname[sizeof(pPkgHead->qname) - 1] = '\0';
	pPkgHead->itemname[sizeof(pPkgHead->itemname) - 1] = '\0';

	const unsigned short pkglen = pPkgHead->bodysize; // 只是包体长度，不含包头；收包时已校验不超过 MAXMSGLEN
	(this->*handler)(pConn, pMsgHeader, reinterpret_cast<char*>(pPkgHead), pkglen);
}

bool CLogicSocket::noop(lpngx_connection_t, LPSTRUC_MSG_HEADER, char *, unsigned short)
{
	ngx_log_stderr(0, "执行了CLogicSocket::noop()!");
	return true;
}

// 以请求的消息头 + 已改写的包头组装应答并发送；包体须已写入 p_sendbuf
void CLogicSocket::SendReplyLocked(LPSTRUC_MSG_HEADER pMsgHeader, PPKGHEAD pPkgHead, char* p_sendbuf)
{
	memcpy(p_sendbuf, pMsgHeader, kMsgHeaderLen);
	memcpy(p_sendbuf + kMsgHeaderLen, pPkgHead, kPkgHeaderLen);
	msgSend(p_sendbuf);
}

void CLogicSocket::SendReply(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, PPKGHEAD pPkgHead, char* p_sendbuf)
{
	CLock lock(&pConn->logicPorcMutex); // 凡是和本用户有关的访问都互斥
	SendReplyLocked(pMsgHeader, pPkgHead, p_sendbuf);
}

void CLogicSocket::SendHeadReply(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, PPKGHEAD pPkgHead)
{
	pPkgHead->bodysize = 0;
	SendReply(pConn, pMsgHeader, pPkgHead, AllocSendBuf(0));
}

// 连接正在 waitpostdata 则立即发送并停止等待超时定时器，否则放入待发队列（满则丢弃）
void CLogicSocket::DeliverPostLocked(lpngx_connection_t pConn, char* p_sendbuf, const char* name)
{
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
		pConn->EnqueuePost(p_sendbuf, name);
	}
}

bool CLogicSocket::HandleReadQ(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char *pPkgHeader, unsigned short iBodyLength)
{
	if (pPkgHeader == nullptr)
	{
		return false;
	}

	auto* pPkgHead = reinterpret_cast<PPKGHEAD>(pPkgHeader);
	const int bufSize = pPkgHead->datasize;
	if (!IsValidBodySize(bufSize))
	{
		pPkgHead->error = ERROR_RECORDSIZE;
		SendHeadReply(pConn, pMsgHeader, pPkgHead);
		return true;
	}

	char* p_sendbuf = AllocSendBuf(bufSize);
	char ip[sizeof(pPkgHead->ip)] = {0};
	if (ReadQ(pPkgHead->qname, BodyOf(p_sendbuf), bufSize, ip))
	{
		pPkgHead->error = 0;
		pPkgHead->bodysize = bufSize;
		CopyName(pPkgHead->ip, ip);
	}
	else
	{
		pPkgHead->error = GetLastErrorQ();
		pPkgHead->bodysize = 0;
	}

	SendReply(pConn, pMsgHeader, pPkgHead, p_sendbuf);
	return true;
}

bool CLogicSocket::HandleWriteQ(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char *pPkgHeader, unsigned short iBodyLength)
{
	if (pPkgHeader == nullptr)
	{
		return false;
	}

	auto* pPkgHead = reinterpret_cast<PPKGHEAD>(pPkgHeader);
	bool ok = false;
	if (PayloadFits(pPkgHead, iBodyLength))
	{
		ok = WriteQ(pPkgHead->qname, PayloadOf(pPkgHead), pPkgHead->datasize, nullptr);
		pPkgHead->error = ResultCode(ok);
	}
	SendHeadReply(pConn, pMsgHeader, pPkgHead);

	// 发布订阅：队列以队列名作为事件名
	if (ok)
	{
		NotifySubscriber(pPkgHead->qname, PayloadOf(pPkgHead), pPkgHead->datasize);
	}
	return true;
}

bool CLogicSocket::HandleClearQ(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char *pPkgHeader, unsigned short iBodyLength)
{
	if (pPkgHeader == nullptr)
	{
		return false;
	}

	auto* pPkgHead = reinterpret_cast<PPKGHEAD>(pPkgHeader);
	pPkgHead->error = ResultCode(ClearQ(pPkgHead->qname));
	SendHeadReply(pConn, pMsgHeader, pPkgHead);
	return true;
}

bool CLogicSocket::HandleReadB(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char *pPkgHeader, unsigned short iBodyLength)
{
	if (pPkgHeader == nullptr)
	{
		return false;
	}

	auto* pPkgHead = reinterpret_cast<PPKGHEAD>(pPkgHeader);
	const int bufSize = pPkgHead->datasize;
	if (!IsValidBodySize(bufSize))
	{
		pPkgHead->error = ERROR_RECORDSIZE;
		SendHeadReply(pConn, pMsgHeader, pPkgHead);
		return true;
	}

	char* p_sendbuf = AllocSendBuf(bufSize);
	timespec timestamp{};
	if (ReadB(pPkgHead->qname, pPkgHead->itemname, BodyOf(p_sendbuf), bufSize, &timestamp))
	{
		pPkgHead->error = 0;
		pPkgHead->bodysize = bufSize;
		pPkgHead->timestamp = timestamp;
	}
	else
	{
		pPkgHead->error = GetLastErrorQ();
		pPkgHead->bodysize = 0;
	}

	SendReply(pConn, pMsgHeader, pPkgHead, p_sendbuf);
	return true;
}

bool CLogicSocket::HandleWriteB(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char *pPkgHeader, unsigned short iBodyLength)
{
	if (pPkgHeader == nullptr)
	{
		return false;
	}

	auto* pPkgHead = reinterpret_cast<PPKGHEAD>(pPkgHeader);
	bool ok = false;
	if (PayloadFits(pPkgHead, iBodyLength))
	{
		ok = WriteB(pPkgHead->qname, pPkgHead->itemname, PayloadOf(pPkgHead), pPkgHead->datasize, nullptr, 0);
		pPkgHead->error = ResultCode(ok);
	}
	SendHeadReply(pConn, pMsgHeader, pPkgHead);

	// 发布订阅：start 为 1 表示触发发布，0 表示不触发
	if (ok && pPkgHead->start == 1)
	{
		// response_tag 只投递给等待的请求方，不通知普通订阅者
		if (!DeliverResponse(pPkgHead->itemname, PayloadOf(pPkgHead), pPkgHead->datasize))
		{
			NotifySubscriber(pPkgHead->itemname, PayloadOf(pPkgHead), pPkgHead->datasize);
		}
	}
	return true;
}

bool CLogicSocket::HandleReadBString(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char *pPkgHeader, unsigned short iBodyLength)
{
	if (pPkgHeader == nullptr)
	{
		return false;
	}

	auto* pPkgHead = reinterpret_cast<PPKGHEAD>(pPkgHeader);
	// g_buffer 只有 MAXMSGLEN 字节，更长的字符串本就装不进应答包
	const int bufSize = std::min(pPkgHead->datasize, MAXMSGLEN);
	timespec timestamp{};
	int strLen = 0; // 字符串的实际长度
	char* p_sendbuf = nullptr;
	if (ReadB_String2(pPkgHead->qname, pPkgHead->itemname, g_buffer, bufSize, &strLen, &timestamp))
	{
		p_sendbuf = AllocSendBuf(strLen);
		memcpy(BodyOf(p_sendbuf), g_buffer, strLen);

		pPkgHead->error = 0;
		pPkgHead->bodysize = strLen; // 包体长度是实际读取的长度，不是 datasize（用户缓冲区的长度）
		pPkgHead->timestamp = timestamp;
	}
	else
	{
		p_sendbuf = AllocSendBuf(0);

		pPkgHead->error = GetLastErrorQ();
		pPkgHead->bodysize = 0;
	}

	SendReply(pConn, pMsgHeader, pPkgHead, p_sendbuf);
	return true;
}

bool CLogicSocket::HandleWriteBString(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char *pPkgHeader, unsigned short iBodyLength)
{
	if (pPkgHeader == nullptr)
	{
		return false;
	}

	auto* pPkgHead = reinterpret_cast<PPKGHEAD>(pPkgHeader);
	bool ok = false;
	if (PayloadFits(pPkgHead, iBodyLength))
	{
		ok = WriteB_String(pPkgHead->qname, pPkgHead->itemname, PayloadOf(pPkgHead), pPkgHead->datasize, nullptr, 0);
		pPkgHead->error = ResultCode(ok);
	}
	SendHeadReply(pConn, pMsgHeader, pPkgHead);

	// 发布订阅：start 为 1 表示触发发布，0 表示不触发
	if (ok && pPkgHead->start == 1)
	{
		NotifySubscriber(pPkgHead->itemname, PayloadOf(pPkgHead), pPkgHead->datasize);
	}
	return true;
}

bool CLogicSocket::HandleClearB(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength)
{
	if (pPkgHeader == nullptr)
	{
		return false;
	}

	auto* pPkgHead = reinterpret_cast<PPKGHEAD>(pPkgHeader);
	pPkgHead->error = ResultCode(ClearB(pPkgHead->qname));
	SendHeadReply(pConn, pMsgHeader, pPkgHead);
	return true;
}

// 这个函数和windows平台的区别是不返回TAG类型的大小
bool CLogicSocket::HandleSubscribe(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char *pPkgHeader, unsigned short iBodyLength)
{
	if (pPkgHeader == nullptr)
	{
		return false;
	}

	auto* pPkgHead = reinterpret_cast<PPKGHEAD>(pPkgHeader);
	const bool tagOk = IsTimerTag(pPkgHead->itemname) || ExistsB("BOARD", pPkgHead->itemname);

	CLock lock(&pConn->logicPorcMutex); // 凡是和本用户有关的访问都互斥
	if (tagOk)
	{
		pConn->Attach(pPkgHead->itemname);

		EventNode eventnode{};
		eventnode.subscriber = pConn;
		eventnode.eventid = static_cast<EVENTID>(pPkgHead->eventid);
		eventnode.eventarg = pPkgHead->eventarg;
		CopyName(eventnode.eventname, pPkgHead->qname); // 用qname字段保存用户定义的事件名！
		m_subscriber.Attach(pPkgHead->itemname, eventnode);
	}

	pPkgHead->error = tagOk ? 0 : ERROR_TAG_NOT_EXIST;
	pPkgHead->bodysize = 0;
	SendReplyLocked(pMsgHeader, pPkgHead, AllocSendBuf(0));
	return true;
}

bool CLogicSocket::HandlePostWait(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char *pPkgHeader, unsigned short iBodyLength)
{
	if (pPkgHeader == nullptr)
	{
		return false;
	}

	auto* pPkgHead = reinterpret_cast<PPKGHEAD>(pPkgHeader);

	CLock lock(&pConn->logicPorcMutex); // 凡是和本用户有关的访问都互斥

	if (!pConn->m_listPost.empty())
	{
		pConn->m_bWaitingPost = false;
		char *p_sendbuf = pConn->m_listPost.front();
		pConn->m_listPost.pop_front();
		msgSend(p_sendbuf);
		return true;
	}

	const int waitMs = pPkgHead->timeout;
	if (waitMs == -1) // 无限等待
	{
		pConn->m_bWaitingPost = true;
		pConn->m_bWaitingTimeout = false;
	}
	else if (waitMs == 0) // 不等待，立即返回超时
	{
		pConn->m_bWaitingPost = false;
		pConn->m_bWaitingTimeout = false;

		char* p_sendbuf = BuildPacket(pMsgHeader->pConn, pMsgHeader->iCurrsequence, POSTWAIT, nullptr, 0);
		PkgHeadOf(p_sendbuf)->error = ERROR_WAIT_TIMEOUT;
		msgSend(p_sendbuf);
	}
	else
	{
		pConn->m_bWaitingPost = true;
		pConn->m_bWaitingTimeout = true;
		pConn->StartTimeoutTimer(waitMs); // 启动最小堆定时器
	}
	return true;
}

void CLogicSocket::NotifySubscriber(const std::string& tagName, const char *pPkgBody, unsigned short iBodyLength)
{
	// 事件风暴保护由每连接待发队列上限（ngx_connection_s::EnqueuePost）负责，这里不限制订阅者数量
	for (const EventNode& subscriber : m_subscriber.GetSubscriber(tagName))
	{
		auto* pConn = static_cast<lpngx_connection_t>(subscriber.subscriber);
		char* p_sendbuf = BuildPostPacket(pConn, tagName.c_str(), pPkgBody, iBodyLength);

		CLock lock(&pConn->logicPorcMutex); // 凡是和本用户有关的访问都互斥

		switch (subscriber.eventid)
		{
		case EVENTID::DEFAULT:
			DeliverPostLocked(pConn, p_sendbuf, tagName.c_str());
			break;
		case EVENTID::POST_DELAY:
			CopyName(PkgHeadOf(p_sendbuf)->itemname, subscriber.eventname); // 延时事件以用户定义的事件名发布
			g_tm.add_once(subscriber.eventarg, [this](void *arg)
						  {
				char* p_postbuf = static_cast<char*>(arg);
				auto* pPostMsgHeader = reinterpret_cast<LPSTRUC_MSG_HEADER>(p_postbuf);
				lpngx_connection_t pconn = pPostMsgHeader->pConn;

				// 定时器线程中执行，必须与 NotifySubscriber/HandlePostWait/超时回调互斥，否则一次 waitpostdata 可能收到两个包
				CLock postLock(&pconn->logicPorcMutex);
				if (pconn->iCurrsequence != pPostMsgHeader->iCurrsequence)
				{
					// 延时期间连接已断开或已被复用
					CMemory::GetInstance()->FreeMemory(p_postbuf);
					return;
				}
				DeliverPostLocked(pconn, p_postbuf, PkgHeadOf(p_postbuf)->itemname); }, p_sendbuf);
			break;
		default:
			ngx_log_stderr(0, "ERROR:unknown eventid");
			CMemory::GetInstance()->FreeMemory(p_sendbuf);
			break;
		}
	}
}

void CLogicSocket::NotifyTimerSubscriber(const std::string& timerName, const char *pPkgBody, unsigned short iBodyLength)
{
	for (const EventNode& subscriber : m_subscriber.GetSubscriber(timerName))
	{
		auto* pConn = static_cast<lpngx_connection_t>(subscriber.subscriber);
		char* p_sendbuf = BuildPostPacket(pConn, timerName.c_str(), pPkgBody, iBodyLength);

		CLock lock(&pConn->logicPorcMutex); // 凡是和本用户有关的访问都互斥
		DeliverPostLocked(pConn, p_sendbuf, timerName.c_str());
	}
}

void CLogicSocket::CancelSubscribe(lpngx_connection_t pConn, const std::list<std::string> &tagList, const std::list<std::string> &plcTagList)
{
	for (const auto& tag : tagList)
	{
		m_subscriber.Detach(tag, pConn);
	}
	for (const auto& plctag : plcTagList)
	{
		m_subscriber.DetachPlcIoServer(plctag, pConn);
	}
	pConn->ClearTagList();
}

bool CLogicSocket::HandleReadType(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength)
{
	if (pPkgHeader == nullptr)
	{
		return false;
	}

	auto* pPkgHead = reinterpret_cast<PPKGHEAD>(pPkgHeader);
	const int bufSize = pPkgHead->datasize;
	if (!IsValidBodySize(bufSize))
	{
		pPkgHead->error = ERROR_RECORDSIZE;
		pPkgHead->recsize = 0;
		SendHeadReply(pConn, pMsgHeader, pPkgHead);
		return true;
	}

	char* p_sendbuf = AllocSendBuf(bufSize);
	int typesize = 0;
	if (ReadType(pPkgHead->qname, pPkgHead->itemname, BodyOf(p_sendbuf), bufSize, &typesize))
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

	SendReply(pConn, pMsgHeader, pPkgHead, p_sendbuf);
	return true;
}

bool CLogicSocket::HandleCreateItem(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char *pPkgHeader, unsigned short iBodyLength)
{
	if (pPkgHeader == nullptr)
	{
		return false;
	}

	auto* pPkgHead = reinterpret_cast<PPKGHEAD>(pPkgHeader);
	// 包体是可选的类型描述
	pPkgHead->error = ResultCode(CreateItem(pPkgHead->qname, pPkgHead->itemname, pPkgHead->recsize, PayloadOf(pPkgHead), pPkgHead->bodysize));
	SendHeadReply(pConn, pMsgHeader, pPkgHead);
	return true;
}

bool CLogicSocket::HandleDeleteItem(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength)
{
	if (pPkgHeader == nullptr)
	{
		return false;
	}

	auto* pPkgHead = reinterpret_cast<PPKGHEAD>(pPkgHeader);
	const bool ok = DeleteItem(pPkgHead->qname, pPkgHead->itemname);
	if (ok)
	{
		ForgetDeletedTag(pPkgHead->itemname);
	}
	pPkgHead->error = ResultCode(ok);
	SendHeadReply(pConn, pMsgHeader, pPkgHead);
	return true;
}

bool CLogicSocket::HandleRegisterPlcServer(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char *pPkgHeader, unsigned short iBodyLength)
{
	if (pPkgHeader == nullptr)
	{
		return false;
	}

	auto* pPkgHead = reinterpret_cast<PPKGHEAD>(pPkgHeader);

	CLock lock(&pConn->logicPorcMutex); // 凡是和本用户有关的访问都互斥
	pConn->AttachPlcTag(pPkgHead->itemname);
	m_subscriber.AttachPlcIoServer(pPkgHead->itemname, pConn);

	pPkgHead->error = 0;
	pPkgHead->bodysize = 0;
	SendReplyLocked(pMsgHeader, pPkgHead, AllocSendBuf(0));
	return true;
}

// 写 PLC：数据不落 Board，直接转发给注册了该 tag 的 PLC I/O 服务器
bool CLogicSocket::HandleWriteBPlc(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char *pPkgHeader, unsigned short iBodyLength)
{
	if (pPkgHeader == nullptr)
	{
		return false;
	}

	auto* pPkgHead = reinterpret_cast<PPKGHEAD>(pPkgHeader);
	if (!PayloadFits(pPkgHead, iBodyLength))
	{
		SendHeadReply(pConn, pMsgHeader, pPkgHead);
		return true;
	}

	pPkgHead->error = 0; // 先假设写PLC成功了，等发布订阅的时候再根据实际情况修改这个值
	SendHeadReply(pConn, pMsgHeader, pPkgHead);

	NotifyPlcIoSever(pPkgHead->itemname, PayloadOf(pPkgHead), pPkgHead->datasize);
	return true;
}

bool CLogicSocket::HandleWriteBStringPlc(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char *pPkgHeader, unsigned short iBodyLength)
{
	// 与 HandleWriteBPlc 相同：字符串也只是原样转发给 PLC I/O 服务器
	return HandleWriteBPlc(pConn, pMsgHeader, pPkgHeader, iBodyLength);
}

void CLogicSocket::NotifyPlcIoSever(const std::string& tagName, const char *pPkgBody, unsigned short iBodyLength)
{
	auto* pConn = static_cast<lpngx_connection_t>(m_subscriber.GetPlcIoServer(tagName));
	if (pConn == nullptr)
	{
		ngx_log_stderr(0, "ERROR:没有PLCIO服务器订阅这个TAG，请检查应用程序,%s这个TAG没有PLCIO服务器订阅了", tagName.c_str());
		return;
	}

	char* p_sendbuf = BuildPostPacket(pConn, tagName.c_str(), pPkgBody, iBodyLength);

	CLock lock(&pConn->logicPorcMutex); // 凡是和本用户有关的访问都互斥
	DeliverPostLocked(pConn, p_sendbuf, tagName.c_str());
}

bool CLogicSocket::HandleReadBoardInfo(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength)
{
	if (pPkgHeader == nullptr)
	{
		return false;
	}

	auto* pPkgHead = reinterpret_cast<PPKGHEAD>(pPkgHeader);
	if (pPkgHead->datasize != sizeof(BOARD_INFO))
	{
		pPkgHead->error = ERROR_RECORDSIZE;
		SendHeadReply(pConn, pMsgHeader, pPkgHead);
		return true;
	}

	char* p_sendbuf = AllocSendBuf(sizeof(BOARD_INFO));
	if (ReadBoardInfo(pPkgHead->qname, reinterpret_cast<BOARD_INFO*>(BodyOf(p_sendbuf))))
	{
		pPkgHead->error = 0;
		pPkgHead->bodysize = sizeof(BOARD_INFO);
	}
	else
	{
		pPkgHead->error = GetLastErrorQ();
		pPkgHead->bodysize = 0;
	}

	SendReply(pConn, pMsgHeader, pPkgHead, p_sendbuf);
	return true;
}

// 请求包：qname=队列名, recsize=记录大小, count=记录数, start=操作模式；包体为可选的类型描述
bool CLogicSocket::HandleCreateQueue(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength)
{
	if (pPkgHeader == nullptr)
	{
		return false;
	}

	auto* pPkgHead = reinterpret_cast<PPKGHEAD>(pPkgHeader);
	pPkgHead->error = ResultCode(CreateAndLoadQ(pPkgHead->qname, pPkgHead->recsize, pPkgHead->count, 0, pPkgHead->start,
												PayloadOf(pPkgHead), pPkgHead->bodysize));
	SendHeadReply(pConn, pMsgHeader, pPkgHead);
	return true;
}

bool CLogicSocket::HandleReadHead(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength)
{
	if (pPkgHeader == nullptr)
	{
		return false;
	}

	auto* pPkgHead = reinterpret_cast<PPKGHEAD>(pPkgHeader);
	char* p_sendbuf = AllocSendBuf(sizeof(QUEUE_HEAD));
	if (ReadHead(pPkgHead->qname, BodyOf(p_sendbuf)))
	{
		pPkgHead->error = 0;
		pPkgHead->bodysize = sizeof(QUEUE_HEAD);
	}
	else
	{
		pPkgHead->error = GetLastErrorQ();
		pPkgHead->bodysize = 0;
	}

	SendReply(pConn, pMsgHeader, pPkgHead, p_sendbuf);
	return true;
}

// 请求包：qname=队列名, start=PEEK_NEXT|PEEK_LATEST, datasize=记录大小；响应包体：RECORD_HEAD + 记录
bool CLogicSocket::HandlePeekQ(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength)
{
	if (pPkgHeader == nullptr)
	{
		return false;
	}

	auto* pPkgHead = reinterpret_cast<PPKGHEAD>(pPkgHeader);
	const int recordSize = pPkgHead->datasize;
	if (recordSize <= 0 || recordSize > MAXMSGLEN - static_cast<int>(sizeof(RECORD_HEAD)))
	{
		pPkgHead->error = ERROR_RECORDSIZE;
		SendHeadReply(pConn, pMsgHeader, pPkgHead);
		return true;
	}

	const int iLenPkgBody = static_cast<int>(sizeof(RECORD_HEAD)) + recordSize;
	char* p_sendbuf = AllocSendBuf(iLenPkgBody);
	char* p_body = BodyOf(p_sendbuf);
	if (PeekQRecord(pPkgHead->qname, pPkgHead->start, p_body + sizeof(RECORD_HEAD), recordSize, reinterpret_cast<RECORD_HEAD*>(p_body)))
	{
		pPkgHead->error = 0;
		pPkgHead->bodysize = iLenPkgBody;
	}
	else
	{
		pPkgHead->error = GetLastErrorQ();
		pPkgHead->bodysize = 0;
	}

	SendReply(pConn, pMsgHeader, pPkgHead, p_sendbuf);
	return true;
}

// 响应包体：以 '\0' 结尾的队列名依次排列，count=队列个数
bool CLogicSocket::HandleListQ(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength)
{
	if (pPkgHeader == nullptr)
	{
		return false;
	}

	auto* pPkgHead = reinterpret_cast<PPKGHEAD>(pPkgHeader);
	char* p_sendbuf = AllocSendBuf(MAXMSGLEN);
	int namesSize = 0;
	int count = 0;
	if (ListQ(BodyOf(p_sendbuf), MAXMSGLEN, &namesSize, &count))
	{
		pPkgHead->error = 0;
		pPkgHead->bodysize = namesSize;
		pPkgHead->count = count;
	}
	else
	{
		pPkgHead->error = GetLastErrorQ();
		pPkgHead->bodysize = 0;
		pPkgHead->count = 0;
	}

	SendReply(pConn, pMsgHeader, pPkgHead, p_sendbuf);
	return true;
}

// 请求包：qname=BOARD, start=起始索引槽；响应包体：count 条 TAG_META 记录，start=下一页起始槽（-1 表示结束）
bool CLogicSocket::HandleListTags(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength)
{
	if (pPkgHeader == nullptr)
	{
		return false;
	}

	auto* pPkgHead = reinterpret_cast<PPKGHEAD>(pPkgHeader);
	char* p_sendbuf = AllocSendBuf(MAXMSGLEN);
	int bytes = 0;
	int count = 0;
	int next = -1;
	if (ListTags(pPkgHead->qname, pPkgHead->start, BodyOf(p_sendbuf), MAXMSGLEN, &bytes, &count, &next))
	{
		pPkgHead->error = 0;
		pPkgHead->bodysize = bytes;
		pPkgHead->count = count;
		pPkgHead->start = next;
	}
	else
	{
		pPkgHead->error = GetLastErrorQ();
		pPkgHead->bodysize = 0;
		pPkgHead->count = 0;
		pPkgHead->start = -1;
	}

	SendReply(pConn, pMsgHeader, pPkgHead, p_sendbuf);
	return true;
}

// 请求包：itemname=request_tag, qname=response_tag, datasize=请求大小, recsize=响应大小, timeout=超时毫秒
bool CLogicSocket::HandleGetResponse(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength)
{
	if (pPkgHeader == nullptr)
	{
		return false;
	}

	auto* pPkgHead = reinterpret_cast<PPKGHEAD>(pPkgHeader);

	auto req = std::make_shared<PendingRequest>();
	req->pConn = pConn;
	req->iCurrsequence = pMsgHeader->iCurrsequence;
	req->requestTag = pPkgHead->itemname;
	req->responseTag = pPkgHead->qname;
	req->responseSize = pPkgHead->recsize;

	const int requestSize = pPkgHead->datasize;
	const int timeout = pPkgHead->timeout;
	unsigned int error = 0;

	if (req->requestTag.empty() || req->responseTag.empty() || req->requestTag == req->responseTag ||
		timeout <= 0 || requestSize <= 0 || requestSize != iBodyLength ||
		req->responseSize <= 0 || req->responseSize > MAXMSGLEN)
	{
		error = ERROR_INVALID_PARAMETER;
	}
	// 借助 ReadB 校验两个 tag 存在且大小一致
	else if (!ReadB("BOARD", req->requestTag.c_str(), g_buffer, requestSize, nullptr) ||
			 !ReadB("BOARD", req->responseTag.c_str(), g_buffer, req->responseSize, nullptr))
	{
		error = GetLastErrorQ();
	}

	if (error != 0)
	{
		SendGetResponseReply(req, error, nullptr, 0);
		return true;
	}

	const char* pData = PayloadOf(pPkgHead);
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
		if (WriteB("BOARD", req->requestTag.c_str(), req->requestData.data(), static_cast<int>(req->requestData.size()), nullptr, 0))
		{
			NotifySubscriber(req->requestTag, req->requestData.data(), static_cast<unsigned short>(req->requestData.size()));
			return;
		}

		const unsigned int error = GetLastErrorQ();
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
bool CLogicSocket::DeliverResponse(const char* responseTag, const char* pData, int iDataLen)
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

// tag 被删除后清理 m_mapResponseOwner：作为 response_tag 的条目；作为 request_tag 且没有挂起请求时其名下的条目
void CLogicSocket::ForgetDeletedTag(const char* tagName)
{
	std::lock_guard<std::mutex> lock(m_reqMutex);

	const bool requestPending = m_mapReqChannel.find(tagName) != m_mapReqChannel.end();
	for (auto it = m_mapResponseOwner.begin(); it != m_mapResponseOwner.end();)
	{
		if (it->first == tagName || (!requestPending && it->second == tagName))
		{
			it = m_mapResponseOwner.erase(it);
		}
		else
		{
			++it;
		}
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
	char* p_sendbuf = BuildPacket(req->pConn, req->iCurrsequence, GETRESPONSE, pBody, iBodyLen);
	PPKGHEAD pPkgHead = PkgHeadOf(p_sendbuf);
	CopyName(pPkgHead->itemname, req->requestTag.c_str());
	CopyName(pPkgHead->qname, req->responseTag.c_str());
	pPkgHead->error = error;

	msgSend(p_sendbuf);
}

// 连接断开时释放其占用的请求，并继续处理排队的下一个
void CLogicSocket::CancelRequest(lpngx_connection_t pConn)
{
	std::vector<PendingRequestPtr> nextList;
	{
		std::lock_guard<std::mutex> lock(m_reqMutex);

		std::vector<PendingRequestPtr> owned;
		for (const auto& [tag, channel] : m_mapReqChannel)
		{
			if (channel.active && channel.active->pConn == pConn)
			{
				owned.push_back(channel.active);
			}
			for (const auto& w : channel.waiting)
			{
				if (w->pConn == pConn)
				{
					owned.push_back(w);
				}
			}
		}

		for (const auto& req : owned)
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
