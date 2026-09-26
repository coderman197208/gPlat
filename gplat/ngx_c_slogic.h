#ifndef __NGX_C_SLOGIC_H__
#define __NGX_C_SLOGIC_H__

#include <sys/socket.h>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "ngx_c_socket.h"

#include "CSubscribe.h"

#define REQUEST_QUEUE_MAX 64	// 每个 request_tag 的最大排队数（不含正在处理的）

struct PendingRequest
{
	lpngx_connection_t pConn;
	uint64_t iCurrsequence;
	std::string requestTag;
	std::string responseTag;
	std::vector<char> requestData;
	int responseSize;
	int timerId{ -1 };
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
	CLogicSocket();
	virtual ~CLogicSocket();
	virtual bool Initialize();

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

	void NotifySubscriber(std::string tagName, char* pPkgBody, unsigned short iBodyLength);
	void NotifyTimerSubscriber(std::string timerName, char* pPkgBody, unsigned short iBodyLength);
	void CancelSubscribe(lpngx_connection_t pConn, const std::list<std::string>& tagList, const std::list<std::string>& plcTagList);

	bool HandleRegisterPlcServer(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength);
	bool HandleWriteBPlc(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength);
	bool HandleWriteBStringPlc(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength);
	void NotifyPlcIoSever(std::string tagName, char* pPkgBody, unsigned short iBodyLength);

	bool HandleGetResponse(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char* pPkgHeader, unsigned short iBodyLength);
	void CancelRequest(lpngx_connection_t pConn);

public:
	virtual void threadRecvProcFunc(char *pMsgBuf);

	CSubscribe m_subscriber;

private:
	void StartRequest(PendingRequestPtr req);
	bool DeliverResponse(const char* responseTag, char* pData, int iDataLen);
	void OnRequestTimeout(const std::weak_ptr<PendingRequest>& weakReq);
	bool DetachRequestLocked(const PendingRequestPtr& req, PendingRequestPtr& next);
	void SendGetResponseReply(const PendingRequestPtr& req, unsigned int error, const char* pBody, int iBodyLen);

	std::mutex m_reqMutex;									// 保护下面两个表；持有期间禁止再获取连接锁 logicPorcMutex
	std::map<std::string, RequestChannel> m_mapReqChannel;	// request_tag -> 请求通道
	std::map<std::string, std::string> m_mapResponseOwner;	// 出现过的 response_tag -> request_tag
};

#endif
