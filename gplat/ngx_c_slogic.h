#ifndef __NGX_C_SLOGIC_H__
#define __NGX_C_SLOGIC_H__

#include <sys/socket.h>
#include <cstddef>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "ngx_c_socket.h"

#include "CSubscribe.h"

inline constexpr std::size_t REQUEST_QUEUE_MAX = 64;	// 每个 request_tag 的最大排队数（不含正在处理的）

struct PendingRequest
{
	lpngx_connection_t pConn{ nullptr };
	uint64_t iCurrsequence{ 0 };
	std::string requestTag;
	std::string responseTag;
	std::vector<char> requestData;
	int responseSize{ 0 };
	TimerManager::TimerId timerId{ -1 };
};
using PendingRequestPtr = std::shared_ptr<PendingRequest>;

struct RequestChannel
{
	PendingRequestPtr active;				// 正在等待响应的请求
	std::deque<PendingRequestPtr> waiting;	// 排队中的同名请求
};

//处理逻辑和通讯的子类
class CLogicSocket : public CSocekt
{
public:
	CLogicSocket() = default;
	~CLogicSocket() override = default;
	bool Initialize() override;

public:
	bool noop(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength);
	bool HandleReadQ(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength);
	bool HandleWriteQ(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength);
	bool HandleClearQ(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength);
	bool HandleReadB(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength);
	bool HandleWriteB(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength);
	bool HandleReadBString(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength);
	bool HandleWriteBString(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength);
	bool HandleClearB(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength);
	bool HandleSubscribe(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength);
	bool HandleReadType(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength);
	bool HandleCreateItem(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength);
	bool HandleDeleteItem(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength);
	bool HandlePostWait(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength);
	bool HandleReadBoardInfo(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength);
	bool HandleCreateQueue(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength);
	bool HandleReadHead(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength);
	bool HandlePeekQ(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength);
	bool HandleListQ(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength);
	bool HandleListTags(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength);

	void NotifySubscriber(const std::string& tagName, const char* pPkgBody, unsigned short iBodyLength);
	void NotifyTimerSubscriber(const std::string& timerName, const char* pPkgBody, unsigned short iBodyLength);
	void CancelSubscribe(lpngx_connection_t pConn, const std::list<std::string>& tagList, const std::list<std::string>& plcTagList) override;

	bool HandleRegisterPlcServer(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength);
	bool HandleWriteBPlc(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength);
	bool HandleWriteBStringPlc(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength);
	void NotifyPlcIoSever(const std::string& tagName, const char* pPkgBody, unsigned short iBodyLength);

	bool HandleGetResponse(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength);
	void CancelRequest(lpngx_connection_t pConn) override;

public:
	void threadRecvProcFunc(char *pMsgBuf) override;

	CSubscribe m_subscriber;

private:
	// 应答：以请求的消息头 + 已改写的包头组装并发送（包体须已写入 p_sendbuf）
	void SendReply(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, PPKGHEAD pPkgHead, char* p_sendbuf);
	void SendReplyLocked(LPSTRUC_MSG_HEADER pMsgHeader, PPKGHEAD pPkgHead, char* p_sendbuf);		// 调用者须持有 logicPorcMutex
	void SendHeadReply(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, PPKGHEAD pPkgHead);	// 无包体应答
	// 投递发布事件包；调用者须持有 pConn->logicPorcMutex。返回 false 表示事件所属的连接实例已不存在（已丢弃）
	bool DeliverPostLocked(lpngx_connection_t pConn, char* p_sendbuf, const char* name);

	void StartRequest(PendingRequestPtr req);
	bool DeliverResponse(const char* responseTag, const char* pData, int iDataLen);
	void OnRequestTimeout(const std::weak_ptr<PendingRequest>& weakReq);
	void ForgetDeletedTag(const char* tagName);
	bool DetachRequestLocked(const PendingRequestPtr& req, PendingRequestPtr& next);
	void SendGetResponseReply(const PendingRequestPtr& req, unsigned int error, const char* pBody, int iBodyLen);

	std::mutex m_reqMutex;									// 保护下面两个表；持有期间禁止再获取连接锁 logicPorcMutex
	std::map<std::string, RequestChannel> m_mapReqChannel;	// request_tag -> 请求通道
	std::map<std::string, std::string> m_mapResponseOwner;	// 出现过的 response_tag -> request_tag
};

#endif
