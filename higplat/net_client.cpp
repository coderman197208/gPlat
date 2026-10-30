// Network client API: blocking TCP requests to the gplat server.
// Wire format: request = MSGHEAD + bodysize bytes of body; response = MSGHEAD + bodysize bytes of body.
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>	// TCP_NODELAY
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../include/msg.h"
#include "error.h"

static_assert(GPLAT_MAX_DATA_SIZE == MAXMSGLEN, "GPLAT_MAX_DATA_SIZE mismatch");
static_assert(GPLAT_TAGNAME_SIZE == sizeof(MSGHEAD::itemname), "GPLAT_TAGNAME_SIZE mismatch");

namespace {

// Subscription events, as in the server's CSubscribe
enum class EventId : int {
	Default = 1,
	PostDelay = 2,
	NotEqualZero = 4,
	EqualZero = 8,
};

constexpr int kConnectTimeoutSec = 2;

// close() may overwrite errno; keep the error that made the connect fail
void close_keep_errno(int fd)
{
	const int savedErrno = errno;
	close(fd);
	errno = savedErrno;
}

// On failure returns -1 with errno set (ETIMEDOUT on timeout, the SO_ERROR value when the connect fails asynchronously)
int unblock_connect(const char* ip, int port, int timeoutSec)
{
	sockaddr_in address{};
	address.sin_family = AF_INET;
	inet_pton(AF_INET, ip, &address.sin_addr);
	address.sin_port = htons(port);

	const int sockfd = socket(PF_INET, SOCK_STREAM, 0);
	if (sockfd < 0)
		return -1;
	const int fdopt = fcntl(sockfd, F_GETFL);
	fcntl(sockfd, F_SETFL, fdopt | O_NONBLOCK);

	// TCP 默认的 Nagle 算法会让包头、包体两次 send 中的后一个小包等待前一个的 ACK，叠加对端的
	// Delayed ACK 会带来约 40ms 的延迟；低延迟场景禁用
	int flag = 1;
	setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

	if (connect(sockfd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
		fcntl(sockfd, F_SETFL, fdopt);
		return sockfd;
	}
	if (errno != EINPROGRESS) {
		close_keep_errno(sockfd);
		return -1;
	}

	// poll 无 FD_SETSIZE 限制（select/FD_SET 在 fd>=1024 时越界）
	pollfd pfd{ sockfd, POLLOUT, 0 };
	int ret;
	do {
		ret = poll(&pfd, 1, timeoutSec * 1000);
	} while (ret < 0 && errno == EINTR);
	if (ret <= 0) {
		if (ret == 0)
			errno = ETIMEDOUT;
		close_keep_errno(sockfd);
		return -1;
	}

	if (!(pfd.revents & (POLLOUT | POLLERR | POLLHUP))) {
		close(sockfd);
		errno = EIO;
		return -1;
	}

	int error = 0;
	socklen_t length = sizeof(error);
	if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error, &length) < 0) {
		close_keep_errno(sockfd);
		return -1;
	}
	if (error != 0) {
		close(sockfd);
		errno = error;
		return -1;
	}

	fcntl(sockfd, F_SETFL, fdopt);
	return sockfd;
}

// Returns len, or <= 0 on error / connection closed
ssize_t send_all(int sockfd, const void* buf, size_t len)
{
	size_t total = 0;
	while (total < len) {
		// MSG_NOSIGNAL：对端已关闭时返回 EPIPE，而不是用 SIGPIPE 杀死调用方进程
		const ssize_t sent = send(sockfd, static_cast<const char*>(buf) + total, len - total, MSG_NOSIGNAL);
		if (sent < 0 && errno == EINTR)
			continue;
		if (sent <= 0)
			return sent;
		total += sent;
	}
	return static_cast<ssize_t>(total);
}

// Blocks until len bytes are read; returns len, fewer when the peer closed (EOF), -1 on error
ssize_t readn(int sockfd, void* buf, size_t len)
{
	size_t nleft = len;
	char* ptr = static_cast<char*>(buf);
	while (nleft > 0) {
		const ssize_t nread = read(sockfd, ptr, nleft);
		if (nread < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (nread == 0)
			break;
		nleft -= nread;
		ptr += nread;
	}
	return static_cast<ssize_t>(len - nleft);
}

// sockfd 始终归调用方：网络 API 从不 close 它。字节流不可再信任时（send/recv 失败、对端关闭、响应错位），
// 只 shutdown 连接：服务端随即断开并清理订阅，此后该 fd 上的调用都以 CONNECTION 类错误快速失败；
// fd 号保持有效，直到调用方 disconnectgplat，因此不会误关其它线程新打开的同号 fd。
bool fail_connection(int sockfd, unsigned int* error, unsigned int code)
{
	*error = code;
	shutdown(sockfd, SHUT_RDWR);
	return false;
}

unsigned int io_error()
{
	return errno != 0 ? static_cast<unsigned int>(errno) : ERROR_SOCKET_NOT_CONNECTED;
}

bool send_request(int sockfd, const void* buf, size_t len, unsigned int* error)
{
	if (send_all(sockfd, buf, len) == static_cast<ssize_t>(len))
		return true;
	return fail_connection(sockfd, error, io_error());
}

// 收满 len 字节；对端提前关闭（EOF）报 ERROR_SOCKET_NOT_CONNECTED
bool recv_response(int sockfd, void* buf, size_t len, unsigned int* error)
{
	const ssize_t n = readn(sockfd, buf, len);
	if (n == static_cast<ssize_t>(len))
		return true;
	return fail_connection(sockfd, error, n < 0 ? io_error() : ERROR_SOCKET_NOT_CONNECTED);
}

// 响应包体比调用方缓冲区大：读掉并丢弃，流保持对齐、连接可继续使用，报 ERROR_BUFFER_TOO_SMALL
bool discard_body(int sockfd, int bodysize, unsigned int* error)
{
	if (bodysize < 0 || bodysize > MAXMSGLEN)
		return fail_connection(sockfd, error, ERROR_INVALID_RESPONSE);
	char scratch[1024];
	while (bodysize > 0) {
		const int n = bodysize < static_cast<int>(sizeof(scratch)) ? bodysize : static_cast<int>(sizeof(scratch));
		if (!recv_response(sockfd, scratch, n, error))
			return false;
		bodysize -= n;
	}
	*error = ERROR_BUFFER_TOO_SMALL;
	return false;
}

// ---- request / response helpers ----

// Truncates to the field size, always '\0'-terminated
template <std::size_t N>
void copy_name(char (&dst)[N], const char* src)
{
	strncpy(dst, src, N - 1);
	dst[N - 1] = '\0';
}

MSGHEAD make_head(MSGID id)
{
	MSGHEAD head{};
	head.id = id;
	return head;
}

// Request on a tag of the (only) board
MSGHEAD make_board_head(MSGID id, const char* tagname)
{
	MSGHEAD head = make_head(id);
	copy_name(head.qname, "BOARD");
	copy_name(head.itemname, tagname);
	return head;
}

// Sends head, with bodysize set, and the body (if any), then receives the response head into head
bool transact(int sockfd, MSGHEAD& head, const void* body, int bodysize, unsigned int* error)
{
	head.bodysize = bodysize;
	return send_request(sockfd, &head, sizeof(MSGHEAD), error)
		&& (bodysize <= 0 || send_request(sockfd, body, bodysize, error))
		&& recv_response(sockfd, &head, sizeof(MSGHEAD), error);
}

// Response without body: the result is the server's error code
bool recv_status(int sockfd, const MSGHEAD& head, unsigned int* error)
{
	if (head.bodysize > 0)
		return fail_connection(sockfd, error, ERROR_INVALID_RESPONSE);
	*error = head.error;
	return *error == 0;
}

// Like recv_status, and the response must echo the request id
bool recv_ack(int sockfd, const MSGHEAD& head, MSGID id, unsigned int* error)
{
	if (head.id != id || head.bodysize != 0)
		return fail_connection(sockfd, error, ERROR_INVALID_RESPONSE);
	*error = head.error;	// 服务端错误（如 ERROR_TAG_NOT_EXIST）不影响连接和已有订阅
	return *error == 0;
}

// Receives the response body into buf; a body larger than size is drained (see discard_body)
bool recv_body(int sockfd, const MSGHEAD& head, void* buf, int size, unsigned int* error)
{
	if (head.bodysize < 0 || head.bodysize > size)
		return discard_body(sockfd, head.bodysize, error);
	return head.bodysize == 0 || recv_response(sockfd, buf, head.bodysize, error);
}

// WRITEB / WRITEBPLC; start = 1 makes the server notify subscribers
bool write_board(int sockfd, MSGID id, const char* tagname, const void* value, int actsize, int start, unsigned int* error)
{
	if (error == nullptr)
		return false;
	if (!tagname || !value || actsize <= 0) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}
	if (actsize > MAXMSGLEN) {
		*error = ERROR_PARAMETER_SIZE;
		return false;
	}

	MSGHEAD head = make_board_head(id, tagname);
	head.datasize = actsize;
	head.start = start;
	return transact(sockfd, head, value, actsize, error) && recv_status(sockfd, head, error);
}

// WRITEBSTRING / WRITEBSTRINGPLC; start = 1 makes the server notify subscribers
bool write_board_string(int sockfd, MSGID id, const char* tagname, const char* value, int start, unsigned int* error)
{
	if (error == nullptr)
		return false;
	// 必须先于 strlen，避免 value==NULL 时解引用空指针
	if (!tagname || !value) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}

	const size_t length = strlen(value);
	if (length > MAXMSGLEN) {
		*error = ERROR_PARAMETER_SIZE;
		return false;
	}

	MSGHEAD head = make_board_head(id, tagname);
	head.datasize = static_cast<int>(length);
	head.start = start;
	return transact(sockfd, head, value, static_cast<int>(length), error) && recv_status(sockfd, head, error);
}

} // namespace

// 失败返回 -1 并设置 errno，同时经错误钩子上报一次：error 为 errno（参数非法时为 ERROR_INVALID_PARAMETER），
// message 附带 host:port
extern "C" int connectgplat(const char* server, int port)
{
	unsigned int error = 0;
	if (!server || port <= 0 || port > 65535) {
		error = ERROR_INVALID_PARAMETER;
		errno = EINVAL;
	}
	else {
		const int sockfd = unblock_connect(server, port, kConnectTimeoutSec);
		if (sockfd >= 0)
			return sockfd;
		error = io_error();
	}

	const int savedErrno = errno;
	char message[160];
	snprintf(message, sizeof(message), "%s (%s:%d)", GetErrorInfo(error).message, server ? server : "null", port);
	ReportError(error, __func__, message);
	errno = savedErrno;
	return -1;
}

extern "C" void disconnectgplat(int sockfd)
{
	if (sockfd >= 0)
		close(sockfd);
}

// ---- queue ----

extern "C" bool readq(int sockfd, const char* qname, void* record, int actsize, unsigned int* error)
{
	if (error == nullptr)
		return false;

	AutoErrorCheck _checker(error, __func__);
	if (!qname || !record || actsize <= 0) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}

	MSGHEAD head = make_head(READQ);
	head.datasize = actsize;
	copy_name(head.qname, qname);
	if (!transact(sockfd, head, nullptr, 0, error))
		return false;

	*error = head.error;
	return *error == 0 && recv_body(sockfd, head, record, actsize, error);
}

extern "C" bool writeq(int sockfd, const char* qname, void* record, int actsize, unsigned int* error)
{
	if (error == nullptr)
		return false;

	AutoErrorCheck _checker(error, __func__);
	if (!qname || !record || actsize <= 0) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}
	if (actsize > MAXMSGLEN) {
		*error = ERROR_PARAMETER_SIZE;
		return false;
	}

	MSGHEAD head = make_head(WRITEQ);
	head.datasize = actsize;
	copy_name(head.qname, qname);
	return transact(sockfd, head, record, actsize, error) && recv_status(sockfd, head, error);
}

extern "C" bool clearq(int sockfd, const char* qname, unsigned int* error)
{
	if (error == nullptr)
		return false;

	AutoErrorCheck _checker(error, __func__);
	if (!qname) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}

	MSGHEAD head = make_head(CLEARQ);
	copy_name(head.qname, qname);
	return transact(sockfd, head, nullptr, 0, error) && recv_status(sockfd, head, error);
}

extern "C" bool readhead(int sockfd, const char* qname, QUEUE_HEAD* queuehead, unsigned int* error)
{
	if (error == nullptr)
		return false;

	AutoErrorCheck _checker(error, __func__);
	if (!qname || !queuehead) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}

	MSGHEAD head = make_head(READHEAD);
	head.datasize = sizeof(QUEUE_HEAD);
	copy_name(head.qname, qname);
	if (!transact(sockfd, head, nullptr, 0, error))
		return false;

	*error = head.error;
	if (*error != 0)
		return false;
	if (head.bodysize != static_cast<int>(sizeof(QUEUE_HEAD)))
		return fail_connection(sockfd, error, ERROR_INVALID_RESPONSE);
	return recv_response(sockfd, queuehead, sizeof(QUEUE_HEAD), error);
}

// 响应包体：RECORD_HEAD + 记录数据
extern "C" bool peekq(int sockfd, const char* qname, int position, void* record, int actsize, RECORD_HEAD* recordhead, unsigned int* error)
{
	if (error == nullptr)
		return false;

	AutoErrorCheck _checker(error, __func__);
	constexpr int kRecordHeadSize = sizeof(RECORD_HEAD);
	if (!qname || !record || actsize <= 0 || actsize > MAXMSGLEN - kRecordHeadSize) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}

	MSGHEAD head = make_head(PEEKQ);
	head.start = position;
	head.datasize = actsize;
	copy_name(head.qname, qname);
	if (!transact(sockfd, head, nullptr, 0, error))
		return false;

	*error = head.error;
	if (*error != 0)
		return false;
	if (head.bodysize < kRecordHeadSize || head.bodysize - kRecordHeadSize > actsize)
		return fail_connection(sockfd, error, ERROR_INVALID_RESPONSE);

	RECORD_HEAD received;
	if (!recv_response(sockfd, &received, kRecordHeadSize, error)
		|| !recv_response(sockfd, record, head.bodysize - kRecordHeadSize, error))
		return false;
	if (recordhead != nullptr)
		*recordhead = received;
	return true;
}

// names 中依次存放以 '\0' 结尾的队列名
extern "C" bool listq(int sockfd, char* names, int buffsize, int* count, unsigned int* error)
{
	if (error == nullptr)
		return false;

	AutoErrorCheck _checker(error, __func__);
	if (!names || !count || buffsize <= 0) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}

	MSGHEAD head = make_head(LISTQ);
	if (!transact(sockfd, head, nullptr, 0, error))
		return false;

	*error = head.error;
	if (*error != 0 || !recv_body(sockfd, head, names, buffsize, error))
		return false;
	*count = head.count;
	return true;
}

extern "C" bool createqueue(int sockfd, const char* queuename, int recordsize, int recordnum, int operatemode, void* type, int typesize, unsigned int* error)
{
	if (error == nullptr)
		return false;

	AutoErrorCheck _checker(error, __func__);
	if (!queuename || recordsize <= 0 || recordnum <= 0 || type == nullptr || typesize <= 0) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}
	if (recordsize > 2048 || typesize > 100) {
		*error = ERROR_PARAMETER_SIZE;
		return false;
	}

	MSGHEAD head = make_head(CREATEQUEUE);
	head.recsize = recordsize;
	head.count = recordnum;
	head.start = operatemode;
	copy_name(head.qname, queuename);
	return transact(sockfd, head, type, typesize, error) && recv_status(sockfd, head, error);
}

// ---- board ----

// 每次返回一页 BOARD tag 元数据，首次 start=0，之后传入上次的 *next，直到 *next == -1
extern "C" bool listtags(int sockfd, int start, char* buff, int buffsize, int* bytes, int* count, int* next, unsigned int* error)
{
	if (error == nullptr)
		return false;

	AutoErrorCheck _checker(error, __func__);
	if (!buff || !bytes || !count || !next || buffsize <= 0 || start < 0) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}

	MSGHEAD head = make_head(LISTTAGS);
	head.start = start;
	copy_name(head.qname, "BOARD");
	if (!transact(sockfd, head, nullptr, 0, error))
		return false;

	*error = head.error;
	if (*error != 0 || !recv_body(sockfd, head, buff, buffsize, error))
		return false;
	*bytes = head.bodysize;
	*count = head.count;
	*next = head.start;
	return true;
}

extern "C" bool readb(int sockfd, const char* tagname, void* value, int actsize, unsigned int* error, timespec* timestamp)
{
	if (error == nullptr)
		return false;

	AutoErrorCheck _checker(error, __func__);
	if (!tagname || !value || actsize <= 0) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}

	MSGHEAD head = make_board_head(READB, tagname);
	head.datasize = actsize;
	if (!transact(sockfd, head, nullptr, 0, error))
		return false;

	*error = head.error;
	if (*error != 0 || !recv_body(sockfd, head, value, actsize, error))
		return false;
	if (timestamp)
		*timestamp = head.timestamp;
	return true;
}

extern "C" bool writeb(int sockfd, const char* tagname, void* value, int actsize, unsigned int* error)
{
	AutoErrorCheck _checker(error, __func__);
	return write_board(sockfd, WRITEB, tagname, value, actsize, 1, error);	// 触发发布
}

extern "C" bool writeb_notpost(int sockfd, const char* tagname, void* value, int actsize, unsigned int* error)
{
	AutoErrorCheck _checker(error, __func__);
	return write_board(sockfd, WRITEB, tagname, value, actsize, 0, error);	// 不触发发布
}

// The string is always '\0'-terminated within buffersize
extern "C" bool readb_string(int sockfd, const char* tagname, char* value, int buffersize, unsigned int* error, timespec* timestamp)
{
	if (error == nullptr)
		return false;

	AutoErrorCheck _checker(error, __func__);
	if (!tagname || !value || buffersize <= 0) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}

	MSGHEAD head = make_board_head(READBSTRING, tagname);
	head.datasize = buffersize;
	if (!transact(sockfd, head, nullptr, 0, error))
		return false;

	// 服务端保证包体不超过 buffersize：放不下时以 BUFFER_TOO_SMALL 返回、不带包体
	*error = head.error;
	if (*error != 0 || !recv_body(sockfd, head, value, buffersize, error))
		return false;

	value[head.bodysize < buffersize ? head.bodysize : buffersize - 1] = '\0';
	if (timestamp)
		*timestamp = head.timestamp;
	return true;
}

extern "C" bool writeb_string(int sockfd, const char* tagname, const char* value, unsigned int* error)
{
	AutoErrorCheck _checker(error, __func__);
	return write_board_string(sockfd, WRITEBSTRING, tagname, value, 1, error);	// 触发发布
}

extern "C" bool writeb_string_notpost(int sockfd, const char* tagname, const char* value, unsigned int* error)
{
	AutoErrorCheck _checker(error, __func__);
	return write_board_string(sockfd, WRITEBSTRING, tagname, value, 0, error);	// 不触发发布
}

extern "C" bool createtag(int sockfd, const char* tagname, int tagsize, void* type, int typesize, unsigned int* error)
{
	if (error == nullptr)
		return false;

	AutoErrorCheck _checker(error, __func__);
	if (!tagname || !type) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}
	// typesize<=0 在发送前拒绝（<0 会让 send_all 以超大 size_t 越界读）
	if (tagsize < 0 || typesize <= 0 || typesize > 100) {
		*error = ERROR_PARAMETER_SIZE;
		return false;
	}

	MSGHEAD head = make_board_head(CREATEITEM, tagname);
	head.recsize = tagsize;
	return transact(sockfd, head, type, typesize, error) && recv_status(sockfd, head, error);
}

extern "C" bool deletetag(int sockfd, const char* tagname, unsigned int* error)
{
	if (error == nullptr)
		return false;

	AutoErrorCheck _checker(error, __func__);
	if (!tagname) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}

	MSGHEAD head = make_board_head(DELETEITEM, tagname);
	return transact(sockfd, head, nullptr, 0, error) && recv_status(sockfd, head, error);
}

extern "C" bool clearb(int sockfd, unsigned int* error)
{
	if (error == nullptr)
		return false;

	AutoErrorCheck _checker(error, __func__);
	MSGHEAD head = make_head(CLEARB);
	copy_name(head.qname, "BOARD");
	return transact(sockfd, head, nullptr, 0, error) && recv_status(sockfd, head, error);
}

extern "C" bool readboardinfo(int sockfd, void* info, int infosize, unsigned int* error)
{
	if (error == nullptr)
		return false;

	AutoErrorCheck _checker(error, __func__);
	if (!info || infosize <= 0) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}

	MSGHEAD head = make_head(READBOARDINFO);
	head.datasize = infosize;
	copy_name(head.qname, "BOARD");
	if (!transact(sockfd, head, nullptr, 0, error))
		return false;

	*error = head.error;
	return *error == 0 && recv_body(sockfd, head, info, infosize, error);
}

// Type descriptor of a queue (qbdname) or, when tagname is non-empty, of a board tag
extern "C" bool readtype(int sockfd, const char* qbdname, const char* tagname, void* inbuff, int buffsize, int* ptypesize, unsigned int* error)
{
	if (error == nullptr)
		return false;

	AutoErrorCheck _checker(error, __func__);
	if ((!tagname && !qbdname) || !inbuff || !ptypesize || buffsize <= 0) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}

	MSGHEAD head = make_head(READTYPE);
	head.datasize = buffsize;
	if (qbdname != nullptr)
		copy_name(head.qname, qbdname);
	if (tagname != nullptr)
		copy_name(head.itemname, tagname);
	if (tagname != nullptr && tagname[0] != '\0')
		copy_name(head.qname, "BOARD");	// 只有一个board
	if (!transact(sockfd, head, nullptr, 0, error))
		return false;

	*ptypesize = head.bodysize;
	*error = head.error;
	if (*error != 0 || head.bodysize == 0)
		return false;
	return recv_body(sockfd, head, inbuff, buffsize, error);
}

// ---- pub/sub ----

extern "C" bool subscribe(int sockfd, const char* tagname, unsigned int* error)
{
	if (error == nullptr)
		return false;

	AutoErrorCheck _checker(error, __func__);
	if (!tagname) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}

	MSGHEAD head = make_head(SUBSCRIBE);
	head.eventid = static_cast<int>(EventId::Default);
	copy_name(head.itemname, tagname);
	copy_name(head.qname, tagname);
	return transact(sockfd, head, nullptr, 0, error) && recv_ack(sockfd, head, SUBSCRIBE, error);
}

// eventname is posted delaytime after each change of tagname
extern "C" bool subscribedelaypost(int sockfd, const char* tagname, const char* eventname, int delaytime, unsigned int* error)
{
	if (error == nullptr)
		return false;

	AutoErrorCheck _checker(error, __func__);
	if (!tagname || !eventname) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}

	MSGHEAD head = make_head(SUBSCRIBE);
	head.eventid = static_cast<int>(EventId::PostDelay);
	head.eventarg = delaytime;
	copy_name(head.itemname, tagname);
	copy_name(head.qname, eventname);
	return transact(sockfd, head, nullptr, 0, error) && recv_status(sockfd, head, error);
}

extern "C" bool waitpostdata(int sockfd, char* tagname, int tagnamesize, void* value, int buffersize, int timeout, unsigned int* error)
{
	if (error == nullptr)
		return false;

	AutoErrorCheck _checker(error, __func__);
	if (sockfd < 0 || tagname == nullptr || tagnamesize < GPLAT_TAGNAME_SIZE || value == nullptr || buffersize <= 0 || timeout < -1) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}
	tagname[0] = '\0';

	// 超时由服务端实现，绝对不能在本地实现，否则容易出现问题
	MSGHEAD head = make_head(POSTWAIT);
	head.timeout = timeout;
	if (!transact(sockfd, head, nullptr, 0, error))
		return false;

	if ((head.id != POST && head.id != POSTWAIT) || head.bodysize < 0)
		return fail_connection(sockfd, error, ERROR_INVALID_RESPONSE);

	// 缓冲区不足：丢弃这条事件，连接和订阅保留
	if (!recv_body(sockfd, head, value, buffersize, error))
		return false;
	if (head.bodysize < buffersize)
		static_cast<char*>(value)[head.bodysize] = '\0';

	*error = head.error;
	if (*error != 0) {
		if (*error == ERROR_WAIT_TIMEOUT) {
			strcpy(tagname, "WAIT_TIMEOUT");
			return true;
		}
		return false;
	}

	head.itemname[sizeof(head.itemname) - 1] = '\0';
	strcpy(tagname, head.itemname);
	return true;
}

// ---- request / response ----

// 请求包：itemname 为请求 tag，qname 为响应 tag，recsize 为响应大小
extern "C" bool getresponse(int sockfd, const char* request_tag, void* request_value, int request_size, const char* response_tag, void* response_value, int response_size, unsigned int* error, int timeout_ms)
{
	if (error == nullptr)
		return false;

	AutoErrorCheck _checker(error, __func__);
	if (sockfd < 0 || !request_tag || !response_tag || !request_value || !response_value ||
		request_size <= 0 || response_size <= 0 || timeout_ms <= 0) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}
	if (request_size > MAXMSGLEN || response_size > MAXMSGLEN) {
		*error = ERROR_PARAMETER_SIZE;
		return false;
	}

	MSGHEAD head = make_head(GETRESPONSE);
	head.datasize = request_size;
	head.recsize = response_size;
	head.timeout = timeout_ms;
	copy_name(head.itemname, request_tag);
	copy_name(head.qname, response_tag);
	if (!transact(sockfd, head, request_value, request_size, error))
		return false;

	if (head.id != GETRESPONSE)
		return fail_connection(sockfd, error, ERROR_INVALID_RESPONSE);

	*error = head.error;
	if (*error != 0)
		return false;
	if (head.bodysize != response_size)
		return fail_connection(sockfd, error, ERROR_INVALID_RESPONSE);
	return recv_response(sockfd, response_value, response_size, error);
}

// ---- PLC I/O server ----

extern "C" bool registertag(int sockfd, const char* tagname, unsigned int* error)
{
	if (error == nullptr)
		return false;

	AutoErrorCheck _checker(error, __func__);
	if (!tagname) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}

	MSGHEAD head = make_head(REGISTERPLCSERVER);
	copy_name(head.itemname, tagname);
	copy_name(head.qname, tagname);
	return transact(sockfd, head, nullptr, 0, error) && recv_ack(sockfd, head, REGISTERPLCSERVER, error);
}

extern "C" bool write_plc_string(int sockfd, const char* tagname, const char* str, unsigned int* error)
{
	AutoErrorCheck _checker(error, __func__);
	return write_board_string(sockfd, WRITEBSTRINGPLC, tagname, str, 0, error);
}

extern "C" bool write_plc_bool(int sockfd, const char* tagname, bool value, unsigned int* error)
{
	AutoErrorCheck _checker(error, __func__);
	return write_board(sockfd, WRITEBPLC, tagname, &value, sizeof(value), 0, error);
}

extern "C" bool write_plc_short(int sockfd, const char* tagname, short value, unsigned int* error)
{
	AutoErrorCheck _checker(error, __func__);
	return write_board(sockfd, WRITEBPLC, tagname, &value, sizeof(value), 0, error);
}

extern "C" bool write_plc_ushort(int sockfd, const char* tagname, unsigned short value, unsigned int* error)
{
	AutoErrorCheck _checker(error, __func__);
	return write_board(sockfd, WRITEBPLC, tagname, &value, sizeof(value), 0, error);
}

extern "C" bool write_plc_int(int sockfd, const char* tagname, int value, unsigned int* error)
{
	AutoErrorCheck _checker(error, __func__);
	return write_board(sockfd, WRITEBPLC, tagname, &value, sizeof(value), 0, error);
}

extern "C" bool write_plc_uint(int sockfd, const char* tagname, unsigned int value, unsigned int* error)
{
	AutoErrorCheck _checker(error, __func__);
	return write_board(sockfd, WRITEBPLC, tagname, &value, sizeof(value), 0, error);
}

extern "C" bool write_plc_float(int sockfd, const char* tagname, float value, unsigned int* error)
{
	AutoErrorCheck _checker(error, __func__);
	return write_board(sockfd, WRITEBPLC, tagname, &value, sizeof(value), 0, error);
}
