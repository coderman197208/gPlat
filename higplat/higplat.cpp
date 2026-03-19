#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <string.h>
#include <netinet/tcp.h>  //TCP_NODELAY

#include "../include/msg.h"
#include "qbd.h"

namespace fs = std::filesystem;

enum EVENTID
{
	DEFAULT = 1,
	POST_DELAY = 2,
	NOT_EQUAL_ZERO = 4,
	EQUAL_ZERO = 8,
};

thread_local char g_buffer[MAXMSGLEN] = { 0 };
thread_local unsigned int errorCode = 0;
char dataQuePath[100] = ".//qbdfile//";

//从环境变量GPLAT_QBD_PATH初始化dataQuePath，共享库加载时自动执行
__attribute__((constructor))
static void init_dataQuePath() {
	const char* env = getenv("GPLAT_QBD_PATH");
	if (env && env[0] != '\0') {
		strncpy(dataQuePath, env, sizeof(dataQuePath) - 2);
		dataQuePath[sizeof(dataQuePath) - 2] = '\0';
		//确保以 / 结尾
		size_t len = strlen(dataQuePath);
		if (len > 0 && dataQuePath[len - 1] != '/') {
			dataQuePath[len] = '/';
			dataQuePath[len + 1] = '\0';
		}
	}
}

//设置QBD文件目录路径
extern "C" void SetQbdPath(const char* path) {
	if (path && path[0] != '\0') {
		strncpy(dataQuePath, path, sizeof(dataQuePath) - 2);
		dataQuePath[sizeof(dataQuePath) - 2] = '\0';
		//确保以 / 结尾
		size_t len = strlen(dataQuePath);
		if (len > 0 && dataQuePath[len - 1] != '/') {
			dataQuePath[len] = '/';
			dataQuePath[len + 1] = '\0';
		}
	}
}

struct TABLE_MSG table[TABLESIZE];
int tabCounter = 0;
std::mutex mutex_rw;
int usereason = 0;

/*F+F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F
Function: gettime

Summary:  Get current time.

Args:     char *timebuf
buffer to save time

Returns:  void
F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F-F*/
inline void gettime(char* timebuf)
{
	struct tm* now;
	time_t ltime;
	time(&ltime);
	now = localtime(&ltime);
	strftime(timebuf, 20, "%Y-%m-%d %H:%M:%S", now);
}

extern "C" {
	int add(int a, int b) {
		return a + b;
	}
}

/*F+F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F
Function: inserttab

Summary:  insert a record into global hash table.

Args:     const struct TABLE_MSG &tabmsg
record to be inserted

Returns:  bool
F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F-F*/
bool inserttab(const struct TABLE_MSG& tabmsg)
{
	int loc, c;
	loc = hash1(tabmsg.dqname);
	c = hash2(tabmsg.dqname);

	//WaitForSingleObject(hTabMutex, INFINITE);
	//std::unique_lock<std::mutex> lock(mutex_rw);

	while (strcmp(table[loc].dqname, "\0") && !table[loc].erased)
	{
		loc = (loc + c) % TABLESIZE;
	}

	if (table[loc].erased)
	{
		strcpy(table[loc].dqname, tabmsg.dqname);
		table[loc].hFile = tabmsg.hFile;
		table[loc].lpMapAddress = tabmsg.lpMapAddress;
		table[loc].hMapFile = tabmsg.hMapFile;
		table[loc].hMutex = tabmsg.hMutex;
		table[loc].pmutex_rw = tabmsg.pmutex_rw;
		table[loc].erased = false;
		table[loc].count++;
		//ReleaseMutex(hTabMutex);
		return true;
	}
	else
	{
		if (tabCounter == TABLESIZE - 1)
		{
			errorCode = ERROR_TABLE_OVERFLOW;
			//ReleaseMutex(hTabMutex);
			return false;
		}
		else
		{
			tabCounter++;
			strcpy(table[loc].dqname, tabmsg.dqname);
			table[loc].hFile = tabmsg.hFile;
			table[loc].lpMapAddress = tabmsg.lpMapAddress;
			table[loc].hMapFile = tabmsg.hMapFile;
			table[loc].hMutex = tabmsg.hMutex;
			table[loc].pmutex_rw = tabmsg.pmutex_rw;
			table[loc].erased = false;
			table[loc].count++;
			//printf("%d count is %d\n", loc, table[loc].count);
			//ReleaseMutex(hTabMutex);
			return true;
		}
	}
}

/*F+F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F
Function: fetchtab

Summary:  fetch a record from global hash table.

Args:     const char *dqname
queue name
struct TABLE_MSG &tabmsg
record to be fetched

Returns:  bool
F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F-F*/
bool fetchtab(const char* dqname, struct TABLE_MSG& tabmsg)
{
	int loc, c;
	loc = hash1(dqname);
	c = hash2(dqname);

	////WaitForSingleObject(hTabMutex, INFINITE);
	std::lock_guard<std::mutex> lock(mutex_rw);

	while (strcmp(table[loc].dqname, "\0") && strcmp(table[loc].dqname, dqname))
	{
		loc = (loc + c) % TABLESIZE;
	}

	if (!strcmp(table[loc].dqname, "\0") || table[loc].erased)
	{
		errorCode = ERROR_RECORD_NOT_EXIST;
		//ReleaseMutex(hTabMutex);
		return false;
	}

	strcpy(tabmsg.dqname, table[loc].dqname);
	tabmsg.hFile = table[loc].hFile;
	tabmsg.lpMapAddress = table[loc].lpMapAddress;
	tabmsg.hMapFile = table[loc].hMapFile;
	tabmsg.hMutex = table[loc].hMutex;
	tabmsg.pmutex_rw = table[loc].pmutex_rw;
	//ReleaseMutex(hTabMutex);
	return true;
}

/*F+F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F
Function: fetchtab1

Summary:  Fetch a record from global hash table, The difference between
fetchtab and fetchtab1 is, when using fetchtab1, if the record
already existed in hash table, its count will be increased by 1.

Args:     const char *dqname
queue name
struct TABLE_MSG &tabmsg
record to be fetched

Returns:  bool
F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F-F*/
bool fetchtab1(const char* dqname, struct TABLE_MSG& tabmsg)
{
	int loc, c;
	loc = hash1(dqname);
	c = hash2(dqname);

	//WaitForSingleObject(hTabMutex, INFINITE);
	//std::lock_guard<std::mutex> lock(mutex_rw);

	while (strcmp(table[loc].dqname, "\0") && strcmp(table[loc].dqname, dqname))
	{
		loc = (loc + c) % TABLESIZE;
	}

	if (!strcmp(table[loc].dqname, "\0") || table[loc].erased)
	{
		errorCode = ERROR_RECORD_NOT_EXIST;
		//ReleaseMutex(hTabMutex);
		return false;
	}

	strcpy(tabmsg.dqname, table[loc].dqname);
	tabmsg.hFile = table[loc].hFile;
	tabmsg.lpMapAddress = table[loc].lpMapAddress;
	tabmsg.hMapFile = table[loc].hMapFile;
	tabmsg.hMutex = table[loc].hMutex;
	tabmsg.pmutex_rw = table[loc].pmutex_rw;
	table[loc].count++;	//gyb
	//ReleaseMutex(hTabMutex);
	return true;
}

/*F+F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F
Function: deletetab

Summary:  Delete a record from global hash table, Decrease the record's
count first, the record is really deleted if the count is zero.

Args:     const char *dqname
queue name
struct TABLE_MSG &tabmsg
record to be deleted

Returns:  bool
F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F-F*/
bool deletetab(const char* dqname, struct TABLE_MSG& tabmsg)
{
	int loc, c;
	loc = hash1(dqname);
	c = hash2(dqname);

	//WaitForSingleObject(hTabMutex, INFINITE);
	//std::unique_lock<std::mutex> lock(mutex_rw);

	while (strcmp(table[loc].dqname, "\0") && strcmp(table[loc].dqname, dqname))
	{
		loc = (loc + c) % TABLESIZE;
	}

	if (!strcmp(table[loc].dqname, "\0") || table[loc].erased)
	{
		errorCode = ERROR_RECORD_NOT_EXIST;
		//ReleaseMutex(hTabMutex);
		return false;
	}

	if (--table[loc].count < 1)
	{
		strcpy(table[loc].dqname, "CT0D9GG_~$59");	 //不是必需的，只是确保安全
		table[loc].erased = true;
		table[loc].count = 0;
		tabmsg.hFile = table[loc].hFile;
		tabmsg.lpMapAddress = table[loc].lpMapAddress;
		tabmsg.hMapFile = table[loc].hMapFile;
		tabmsg.hMutex = table[loc].hMutex;
		tabmsg.pmutex_rw = table[loc].pmutex_rw;
		//ReleaseMutex(hTabMutex);
		return true;
	}
	//ReleaseMutex(hTabMutex);
	return false;
}

/*F+F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F
Function: hash1, hash2

Summary:  Get hash key value.

Args:     const char *s
key string

Returns:  int
F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F-F*/
inline int hash1(const char* s)
{
	int k = 0;
	while (*s)
	{
		k += (int)*s;
		s++;
	}
	return (k % TABLESIZE);
}


inline int hash2(const char* s)
{
	int k = 0, remain;
	while (*s)
	{
		k += (int)*s;
		s++;
	}

	remain = k % (TABLESIZE - 2);
	return remain == 0 ? 1 : remain;
}

int setnonblocking(int fd)
{
	int old_option = fcntl(fd, F_GETFL);
	int new_option = old_option | O_NONBLOCK;
	fcntl(fd, F_SETFL, new_option);
	return old_option;
}

int setblocking(int fd)
{
	int old_option = fcntl(fd, F_GETFL);
	int new_option = old_option & ~O_NONBLOCK;
	fcntl(fd, F_SETFL, new_option);
	return old_option;
}

int unblock_connect(const char* ip, int port, int time)
{
	int ret = 0;
	struct sockaddr_in address;
	bzero(&address, sizeof(address));
	address.sin_family = AF_INET;
	inet_pton(AF_INET, ip, &address.sin_addr);
	address.sin_port = htons(port);

	int sockfd = socket(PF_INET, SOCK_STREAM, 0);
	int fdopt = setnonblocking(sockfd);

	//Nagle 算法（发送方缓冲合并小包）
	//TCP 默认启用 Nagle 算法，它的核心逻辑是：
	//如果发送方有未确认的小数据（小于 MSS，通常 1460 字节），则会等待：
	//直到收到前一个包的 ACK，或者
	//累积到足够大的数据（MSS）再发送。

	//你的情况：两次 send 的小数据可能被 Nagle 算法合并，导致第二个 send 延迟发送。
	//服务器数据接收延迟现象最可能由 Nagle + Delayed ACK 组合 导致：
	//客户端第一个send触发Nagle等待ACK。
	//客户端第二个send因为Delayed ACK的40ms延迟，导致数据晚到。
	//解决办法：发送方禁用 Nagle（适合低延迟场景）
	int flag = 1;
	setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

	ret = connect(sockfd, (struct sockaddr*)&address, sizeof(address));
	if (ret == 0)
	{
		printf("connect with server immediately\n");
		fcntl(sockfd, F_SETFL, fdopt);
		return sockfd;
	}
	else if (errno != EINPROGRESS)
	{
		printf("unblock connect not support\n");
		return -1;
	}

	fd_set readfds;
	fd_set writefds;
	struct timeval timeout;

	FD_ZERO(&readfds);
	FD_SET(sockfd, &writefds);

	timeout.tv_sec = time;
	timeout.tv_usec = 0;

	ret = select(sockfd + 1, NULL, &writefds, NULL, &timeout);
	if (ret <= 0)
	{
		printf("connection time out\n");
		close(sockfd);
		return -1;
	}

	if (!FD_ISSET(sockfd, &writefds))
	{
		printf("no events on sockfd found\n");
		close(sockfd);
		return -1;
	}

	int error = 0;
	socklen_t length = sizeof(error);
	if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error, &length) < 0)
	{
		printf("get socket option failed\n");
		close(sockfd);
		return -1;
	}

	if (error != 0)
	{
		printf("connection failed after select with the error: %d \n", error);
		close(sockfd);
		return -1;
	}

	printf("connection ready after select with the socket: %d \n", sockfd);
	fcntl(sockfd, F_SETFL, fdopt);
	return sockfd;
}

int send_all(int sockfd, const void* buf, size_t len) {
	size_t total_sent = 0;
	while (total_sent < len) {
		//mark send会像下面的readn一样被信号中断吗
		int sent = send(sockfd, (char*)buf + total_sent, len - total_sent, 0);
		if (sent <= 0) return sent; // 错误或连接关闭
		total_sent += sent;
	}
	return total_sent;
}

/**
 * 阻塞读取指定字节数的数据
 * @param sockfd 套接字描述符
 * @param buf 接收缓冲区
 * @param len 需要读取的字节数
 * @return 成功返回实际读取的字节数(等于len)，失败返回-1
 */
ssize_t readn(int sockfd, void* buf, size_t len) {
	size_t nleft = len;    // 剩余需要读取的字节数
	ssize_t nread;         // 每次读取的字节数
	char* ptr = (char*)buf;  // 当前读取位置

	while (nleft > 0) {
		nread = read(sockfd, ptr, nleft);

		if (nread < 0) {
			if (errno == EINTR) {  // 被信号中断
				nread = 0;         // 重新调用read
			}
			else {
				return -1;         // 其他错误
			}
		}
		else if (nread == 0) {   // EOF，对端关闭连接
			break;                  // 返回已读取的字节数(小于len)
		}

		nleft -= nread;
		ptr += nread;
	}

	return (len - nleft);  // 返回实际读取的字节数
}

//变体版本（带超时控制）
ssize_t readn_timeout(int sockfd, void* buf, size_t len, int timeout_sec) {
	fd_set readfds;
	struct timeval tv;
	size_t nleft = len;
	ssize_t nread;
	char* ptr = (char*)buf;

	while (nleft > 0) {
		FD_ZERO(&readfds);
		FD_SET(sockfd, &readfds);
		tv.tv_sec = timeout_sec;
		tv.tv_usec = 0;

		int ret = select(sockfd + 1, &readfds, NULL, NULL, &tv);
		if (ret == -1) {
			if (errno == EINTR) continue;
			return -1;
		}
		else if (ret == 0) {
			errno = ETIMEDOUT;
			return -1;
		}

		nread = read(sockfd, ptr, nleft);
		// 其余处理与普通版本相同...
		if (nread < 0) {
			if (errno == EINTR) {  // 被信号中断
				nread = 0;         // 重新调用read
			}
			else {
				return -1;         // 其他错误
			}
		}
		else if (nread == 0) {   // EOF，对端关闭连接
			break;                  // 返回已读取的字节数(小于len)
		}

		nleft -= nread;
		ptr += nread;
	}

	return (len - nleft);
}

/*F+F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F
Function: IsEmptyQ

Summary:  Get error code.

Args:     void

Returns:  BOOL
F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F-F*/
extern "C" unsigned int GetLastErrorQ()
{
	return errorCode;
}

extern "C" int connectgplat(const char* server, int port)
{
	// 参数校验
	if (!server || port <= 0 || port > 65535) {
		fprintf(stderr, "[ERROR] Invalid server address or port number\n");
		return -1;
	}

	// 建立非阻塞连接
	int sockfd = unblock_connect(server, port, 2);  // 2秒超时
	if (sockfd < 0) {
		fprintf(stderr, "[ERROR] Connection to %s:%d failed: %s\n",
			server, port, strerror(errno));
		return -1;
	}

	// 设置为阻塞模式
	if (!setblocking(sockfd)) {
		fprintf(stderr, "[WARNING] Failed to set blocking mode for socket %d\n", sockfd);
		// 继续使用，非致命错误
	}

	printf("[INFO] Connected to gplat server %s:%d (fd=%d)\n", server, port, sockfd);
	return sockfd;
}

extern "C" void disconnectgplat(int sockfd)
{
	if (sockfd >= 0) {
		close(sockfd);
		printf("[INFO] Disconnected from gplat server (fd=%d)\n", sockfd);
	}
}

extern "C" bool readq(int sockfd, const char* qname, void* record, int actsize, unsigned int* error)
{
	// 参数校验
	if (!qname || !record || !error || actsize <= 0) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}

	// 初始化消息头
	MSGSTRUCT msg{};
	msg.head.id = READQ;
	msg.head.datasize = actsize;
	msg.head.bodysize = 0;

	// 安全拷贝队列名
	strncpy(msg.head.qname, qname, sizeof(msg.head.qname) - 1);
	msg.head.qname[sizeof(msg.head.qname) - 1] = '\0';

	// 发送请求
	if (send_all(sockfd, &msg, sizeof(MSGHEAD)) <= 0) {
		*error = errno;
		close(sockfd);
		return false;
	}

	// 读取响应头
	if (readn(sockfd, &msg, sizeof(MSGHEAD)) < 0) {
		*error = errno;
		close(sockfd);
		return false;
	}

	// 检查错误码
	*error = msg.head.error;
	if (*error != 0) {
		return false;
	}

	// 验证数据大小
	if (msg.head.bodysize > actsize) {
		*error = ERROR_BUFFER_TOO_SMALL;
		close(sockfd);
		return false;
	}

	// 读取数据体（如果有）
	if (msg.head.bodysize > 0) {
		if (readn(sockfd, record, msg.head.bodysize) < 0) {
			*error = errno;
			close(sockfd);
			return false;
		}
	}

	return true;
}

extern "C" bool writeq(int sockfd, const char* qname, void* record, int actsize, unsigned int* error)
{
	// 参数校验
	if (!qname || !record || !error || actsize <= 0) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}

	if (actsize > MAXMSGLEN) {
		*error = ERROR_PARAMETER_SIZE;
		return false;
	}

	// 初始化消息头
	MSGSTRUCT msg{};
	msg.head.id = WRITEQ;
	msg.head.datasize = actsize;
	msg.head.bodysize = actsize;

	// 安全拷贝队列名
	strncpy(msg.head.qname, qname, sizeof(msg.head.qname) - 1);
	msg.head.qname[sizeof(msg.head.qname) - 1] = '\0';

	// 发送消息头和数据
	if (send_all(sockfd, &msg, sizeof(MSGHEAD)) <= 0 ||
		send_all(sockfd, record, actsize) <= 0) {
		*error = errno;
		close(sockfd);
		return false;
	}

	// 读取响应
	if (readn(sockfd, &msg, sizeof(MSGHEAD)) < 0) {
		*error = errno;
		close(sockfd);
		return false;
	}

	// 验证响应
	if (msg.head.bodysize > 0) {
		*error = ERROR_INVALID_RESPONSE;
		close(sockfd);
		return false;
	}

	*error = msg.head.error;
	return (*error == 0);
}

bool clearq(int sockfd, const char* qname, unsigned int* error)
{
	// 参数校验
	if (!qname || !error) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}

	// 初始化消息头
	MSGSTRUCT msg{};
	msg.head.id = CLEARQ;
	msg.head.bodysize = 0;

	// 安全拷贝队列名
	strncpy(msg.head.qname, qname, sizeof(msg.head.qname) - 1);
	msg.head.qname[sizeof(msg.head.qname) - 1] = '\0';

	// 发送请求
	if (send_all(sockfd, &msg, sizeof(MSGHEAD)) <= 0) {
		*error = errno;
		close(sockfd);
		return false;
	}

	// 读取响应头
	if (readn(sockfd, &msg, sizeof(MSGHEAD)) < 0) {
		*error = errno;
		close(sockfd);
		return false;
	}

	// 检查错误码
	*error = msg.head.error;
	if (*error != 0) {
		return false;
	}

	return true;
}

extern "C" bool readb(int sockfd, const char* tagname, void* value, int actsize, unsigned int* error, timespec* timestamp)
{
	// 参数校验
	if (!tagname || !value || !error || actsize <= 0) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}

	// 初始化消息头
	MSGSTRUCT msg{};
	msg.head.id = READB;
	msg.head.datasize = actsize;
	msg.head.bodysize = 0;

	// 安全拷贝字符串
	strncpy(msg.head.qname, "BOARD", sizeof(msg.head.qname) - 1);
	msg.head.qname[sizeof(msg.head.qname) - 1] = '\0';

	strncpy(msg.head.itemname, tagname, sizeof(msg.head.itemname) - 1);
	msg.head.itemname[sizeof(msg.head.itemname) - 1] = '\0';

	// 发送请求
	if (send_all(sockfd, &msg, sizeof(MSGHEAD)) <= 0) {
		*error = errno;
		close(sockfd);
		return false;
	}

	// 读取响应头
	if (readn(sockfd, &msg, sizeof(MSGHEAD)) < 0) {
		*error = errno;
		close(sockfd);
		return false;
	}

	// 检查错误码
	*error = msg.head.error;
	if (*error != 0) {
		return false;
	}

	// 验证数据大小
	if (msg.head.bodysize > actsize) {
		*error = ERROR_BUFFER_TOO_SMALL;
		close(sockfd);
		return false;
	}

	// 读取数据体（如果有）
	if (msg.head.bodysize > 0) {
		if (readn(sockfd, value, msg.head.bodysize) < 0) {
			*error = errno;
			close(sockfd);
			return false;
		}
	}

	// 返回时间戳（如果请求）
	if (timestamp) {
		*timestamp = msg.head.timestamp;
	}

	return true;
}

bool writeb_(int sockfd, const char* tagname, void* value, int actsize, unsigned int* error, int postornot)
{
	// 参数校验
	if (!tagname || !value || !error || actsize <= 0) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}

	if (actsize > MAXMSGLEN) {
		*error = ERROR_PARAMETER_SIZE;
		return false;
	}

	// 初始化消息头
	MSGSTRUCT msg{};
	msg.head.id = WRITEB;
	msg.head.datasize = actsize;
	msg.head.bodysize = actsize;
	msg.head.offset = 0;
	msg.head.subsize = 0;
	msg.head.start = postornot;	// 1表示触发发布，0表示不触发发布

	// 安全拷贝字符串
	strncpy(msg.head.qname, "BOARD", sizeof(msg.head.qname) - 1);
	msg.head.qname[sizeof(msg.head.qname) - 1] = '\0';

	strncpy(msg.head.itemname, tagname, sizeof(msg.head.itemname) - 1);
	msg.head.itemname[sizeof(msg.head.itemname) - 1] = '\0';

	//禁用Nagle算法（适合低延迟场景）后，则执行效率相同
	//memcpy(msg.body, value, actsize);
	//send_all(sockfd, &msg, sizeof(MSGHEAD) + actsize);
	// 发送消息头和数据（单次发送优化）
	if (send_all(sockfd, &msg, sizeof(MSGHEAD)) <= 0 ||
		send_all(sockfd, value, actsize) <= 0) {
		*error = errno;
		close(sockfd);
		return false;
	}

	// 接收响应
	if (readn(sockfd, &msg, sizeof(MSGHEAD)) < 0) {
		*error = errno;
		close(sockfd);
		return false;
	}

	// 验证响应
	if (msg.head.bodysize > 0) {
		*error = ERROR_INVALID_RESPONSE;
		close(sockfd);
		return false;
	}

	*error = msg.head.error;
	return (*error == 0);
}

extern "C" bool writeb(int sockfd, const char* tagname, void* value, int actsize, unsigned int* error)
{
	return writeb_(sockfd, tagname, value, actsize, error, 1);	// 触发发布
}

extern "C" bool writeb_notpost(int sockfd, const char* tagname, void* value, int actsize, unsigned int* error)
{
	return writeb_(sockfd, tagname, value, actsize, error, 0);	// 不触发发布
}

extern "C" bool readb_string(int sockfd, const char* tagname, char* value, int buffersize, unsigned int* error, timespec* timestamp)
{
	// 参数校验
	if (!tagname || !value || !error || buffersize <= 0) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}

	// 初始化消息头
	MSGSTRUCT msg{};
	msg.head.id = READBSTRING;
	msg.head.datasize = buffersize;
	msg.head.bodysize = 0;

	// 安全拷贝字符串
	strncpy(msg.head.qname, "BOARD", sizeof(msg.head.qname) - 1);
	msg.head.qname[sizeof(msg.head.qname) - 1] = '\0';

	strncpy(msg.head.itemname, tagname, sizeof(msg.head.itemname) - 1);
	msg.head.itemname[sizeof(msg.head.itemname) - 1] = '\0';

	// 发送请求
	if (send_all(sockfd, &msg, sizeof(MSGHEAD)) <= 0) {
		*error = errno;
		close(sockfd);
		return false;
	}

	// 读取响应头
	if (readn(sockfd, &msg, sizeof(MSGHEAD)) < 0) {
		*error = errno;
		close(sockfd);
		return false;
	}

	// 检查错误码
	*error = msg.head.error;
	if (*error != 0) {
		return false;
	}

	// 验证数据大小
	// 目前这种情况不可能出现，因为服务器会确保数据体大小不超过buffersize，如果buffsize不够大，上面错误码不为零就会返回，不会走到这里
	if (msg.head.bodysize > buffersize) {
		*error = ERROR_BUFFER_TOO_SMALL;
		close(sockfd);
		return false;
	}

	// 读取数据体（如果有）
	if (msg.head.bodysize > 0) {
		if (readn(sockfd, value, msg.head.bodysize) < 0) {
			*error = errno;
			close(sockfd);
			return false;
		}
	}

	// 确保字符串以 null 结尾
	if (msg.head.bodysize < buffersize) {
		value[msg.head.bodysize] = '\0';  // 确保 null 结尾
	} else {
		value[buffersize - 1] = '\0';  // 防止溢出
	}

	// 返回时间戳（如果请求）
	if (timestamp) {
		*timestamp = msg.head.timestamp;
	}

	return true;
}

extern "C" bool readb_string2(int sockfd, const char* tagname, std::string& value, unsigned int* error, timespec* timestamp = 0)
{
	//if (readb_string(sockfd, tagname, g_buffer, MAXMSGLEN, error, timestamp)) {
	//	// 确保字符串以 null 结尾
	//	g_buffer[MAXMSGLEN - 1] = '\0';
	//	value.assign(g_buffer);  // 使用 std::string 的 assign 方法
	//	return true;
	//}

	// 参数校验
	if (!tagname || !error) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}

	// 初始化消息头
	MSGSTRUCT msg{};
	msg.head.id = READBSTRING;
	msg.head.datasize = MAXMSGLEN;
	msg.head.bodysize = 0;

	// 安全拷贝字符串
	strncpy(msg.head.qname, "BOARD", sizeof(msg.head.qname) - 1);
	msg.head.qname[sizeof(msg.head.qname) - 1] = '\0';

	strncpy(msg.head.itemname, tagname, sizeof(msg.head.itemname) - 1);
	msg.head.itemname[sizeof(msg.head.itemname) - 1] = '\0';

	// 发送请求
	if (send_all(sockfd, &msg, sizeof(MSGHEAD)) <= 0) {
		*error = errno;
		close(sockfd);
		return false;
	}

	// 读取响应头
	if (readn(sockfd, &msg, sizeof(MSGHEAD)) < 0) {
		*error = errno;
		close(sockfd);
		return false;
	}

	// 检查错误码
	*error = msg.head.error;
	if (*error != 0) {
		return false;
	}

	// 验证数据大小
	// 目前这种情况不可能出现，因为服务器会确保数据体大小不超过buffersize，如果buffsize不够大，上面错误码不为零就会返回，不会走到这里
	if (msg.head.bodysize > MAXMSGLEN) {
		*error = ERROR_BUFFER_TOO_SMALL;
		close(sockfd);
		return false;
	}

	// 读取数据体（如果有）
	if (msg.head.bodysize > 0) {
		if (readn(sockfd, g_buffer, msg.head.bodysize) < 0) {
			*error = errno;
			close(sockfd);
			return false;
		}
	}

	// 确保字符串以 null 结尾
	if (msg.head.bodysize < MAXMSGLEN) {
		g_buffer[msg.head.bodysize] = '\0';  // 确保 null 结尾
	}
	else {
		g_buffer[MAXMSGLEN - 1] = '\0';  // 防止溢出
	}

	// 返回时间戳（如果请求）
	if (timestamp) {
		*timestamp = msg.head.timestamp;
	}

	value.assign(g_buffer, msg.head.bodysize);  // 使用 std::string 的 assign 方法
	return true;
}

bool writeb_string_(int sockfd, const char* tagname, const char* value, unsigned int* error, int postornot)
{
	int strlength = strlen((const char*)value);

	// 参数校验
	if (!tagname || !value || !error) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}

	if (strlength > MAXMSGLEN) {
		*error = ERROR_PARAMETER_SIZE;
		return false;
	}

	// 初始化消息头
	MSGSTRUCT msg{};
	msg.head.id = WRITEBSTRING;
	msg.head.datasize = strlength;
	msg.head.bodysize = strlength;
	msg.head.offset = 0;
	msg.head.subsize = 0;
	msg.head.start = postornot;	// 1表示触发发布，0表示不触发发布

	// 安全拷贝字符串
	strncpy(msg.head.qname, "BOARD", sizeof(msg.head.qname) - 1);
	msg.head.qname[sizeof(msg.head.qname) - 1] = '\0';

	strncpy(msg.head.itemname, tagname, sizeof(msg.head.itemname) - 1);
	msg.head.itemname[sizeof(msg.head.itemname) - 1] = '\0';

	if (send_all(sockfd, &msg, sizeof(MSGHEAD)) <= 0 ||
		(strlength > 0 && send_all(sockfd, value, strlength) <= 0)) {
		*error = errno;
		close(sockfd);
		return false;
	}

	// 接收响应
	if (readn(sockfd, &msg, sizeof(MSGHEAD)) < 0) {
		*error = errno;
		close(sockfd);
		return false;
	}

	// 验证响应
	if (msg.head.bodysize > 0) {
		*error = ERROR_INVALID_RESPONSE;
		close(sockfd);
		return false;
	}

	*error = msg.head.error;
	return (*error == 0);
}

extern "C" bool writeb_string(int sockfd, const char* tagname, const char* value, unsigned int* error)
{
	return writeb_string_(sockfd, tagname, value, error, 1);	// 触发发布
}

extern "C" bool writeb_string_notpost(int sockfd, const char* tagname, const char* value, unsigned int* error)
{
	return writeb_string_(sockfd, tagname, value, error, 0);	// 不触发发布
}

extern "C" bool writeb_string2(int sockfd, const char* tagname, std::string value, unsigned int* error)
{
	// 确保字符串以 null 结尾
	if (value.length() >= MAXMSGLEN) {
		*error = ERROR_PARAMETER_SIZE;
		return false;
	}
	// mark
	// 直接传递 c_str()，因为 writeb_string 已经处理了字符串长度和 null 结尾问题
	// 但是这样会损失效率，因为没有利用std::string的length()方法，length()方法比strlen()更快，因为它不需要遍历字符串
	return writeb_string_(sockfd, tagname, value.c_str(), error, 1);		// 触发发布
}

extern "C" bool subscribe(int sockfd, const char* tagname, unsigned int* error)
{
	// 参数校验
	if (!tagname || !error) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}

	// 初始化消息结构体
	MSGSTRUCT msg{};
	msg.head.id = SUBSCRIBE;
	msg.head.eventid = EVENTID::DEFAULT;
	msg.head.eventarg = 0;
	msg.head.bodysize = 0;

	// 安全拷贝字符串（防止缓冲区溢出）
	strncpy(msg.head.itemname, tagname, sizeof(msg.head.itemname) - 1);
	msg.head.itemname[sizeof(msg.head.itemname) - 1] = '\0';

	strncpy(msg.head.qname, tagname, sizeof(msg.head.qname) - 1);
	msg.head.qname[sizeof(msg.head.qname) - 1] = '\0';

	// 发送请求
	if (send_all(sockfd, &msg, sizeof(MSGHEAD)) <= 0) {
		*error = errno;
		close(sockfd);
		return false;
	}

	// 读取响应
	if (readn(sockfd, &msg, sizeof(MSGHEAD)) < 0) {
		*error = errno;
		close(sockfd);
		return false;
	}

	// 验证响应
	if (msg.head.bodysize > 0) {
		*error = ERROR_INVALID_RESPONSE;
		close(sockfd);
		return false;
	}

	*error = msg.head.error;
	return (*error == 0);
}

extern "C" bool clearb(int sockfd, unsigned int* error)
{
	// 参数校验
	if (!error) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}
	// 初始化消息结构体
	MSGSTRUCT msg{};
	msg.head.id = CLEARB;
	msg.head.bodysize = 0;
	// 安全拷贝字符串
	strncpy(msg.head.qname, "BOARD", sizeof(msg.head.qname) - 1);
	msg.head.qname[sizeof(msg.head.qname) - 1] = '\0';
	// 发送请求
	if (send_all(sockfd, &msg, sizeof(MSGHEAD)) <= 0) {
		*error = errno;
		close(sockfd);
		return false;
	}
	// 读取响应
	if (readn(sockfd, &msg, sizeof(MSGHEAD)) < 0) {
		*error = errno;
		close(sockfd);
		return false;
	}
	// 验证响应
	if (msg.head.bodysize > 0) {
		*error = ERROR_INVALID_RESPONSE;
		close(sockfd);
		return false;
	}
	*error = msg.head.error;
	return (*error == 0);
}

extern "C" bool readboardinfo(int sockfd, const void* info, int infosize, unsigned int* error)
{
	// 参数校验
	if (!info || infosize <= 0 || !error) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}
	// 初始化消息结构体
	MSGSTRUCT msg{};
	msg.head.id = READBOARDINFO;
	msg.head.datasize = infosize;
	msg.head.bodysize = 0;
	// 安全拷贝字符串
	strncpy(msg.head.qname, "BOARD", sizeof(msg.head.qname) - 1);
	msg.head.qname[sizeof(msg.head.qname) - 1] = '\0';
	// 发送请求
	if (send_all(sockfd, &msg, sizeof(MSGHEAD)) <= 0) {
		*error = errno;
		close(sockfd);
		return false;
	}
	// 读取响应头
	if (readn(sockfd, &msg, sizeof(MSGHEAD)) < 0) {
		*error = errno;
		close(sockfd);
		return false;
	}
	// 检查错误码
	*error = msg.head.error;
	if (*error != 0) {
		return false;
	}
	// 验证数据大小
	if (msg.head.bodysize > infosize) {
		*error = ERROR_BUFFER_TOO_SMALL;
		close(sockfd);
		return false;
	}
	// 读取数据体（如果有）
	if (msg.head.bodysize > 0) {
		if (readn(sockfd, const_cast<void*>(info), msg.head.bodysize) < 0) {
			*error = errno;
			close(sockfd);
			return false;
		}
	}
	return true;
}

extern "C" bool subscribedelaypost(int sockfd, const char* tagname, const char* eventname, int delaytime, unsigned int* error)
{
	// 参数校验
	if (!tagname || !eventname || !error) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}

	// 初始化消息结构体
	MSGSTRUCT msg{};
	msg.head.id = SUBSCRIBE;
	msg.head.eventid = EVENTID::POST_DELAY;
	msg.head.eventarg = delaytime;
	msg.head.bodysize = 0;

	// 安全拷贝字符串（防止缓冲区溢出）
	strncpy(msg.head.itemname, tagname, sizeof(msg.head.itemname) - 1);
	msg.head.itemname[sizeof(msg.head.itemname) - 1] = '\0';

	strncpy(msg.head.qname, eventname, sizeof(msg.head.qname) - 1);
	msg.head.qname[sizeof(msg.head.qname) - 1] = '\0';

	// 发送请求
	if (send_all(sockfd, &msg, sizeof(MSGHEAD)) <= 0) {
		*error = errno;
		close(sockfd);
		return false;
	}

	// 读取响应
	if (readn(sockfd, &msg, sizeof(MSGHEAD)) < 0) {
		*error = errno;
		close(sockfd);
		return false;
	}

	// 验证响应
	if (msg.head.bodysize > 0) {
		*error = ERROR_INVALID_RESPONSE;
		close(sockfd);
		return false;
	}

	*error = msg.head.error;
	return (*error == 0);
}

extern "C" bool createtag(int sockfd, const char* tagname, int tagsize, void* type, int typesize, unsigned int* error)
{
	// 参数校验
	if (!tagname || !type || !error) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}

	if (typesize > 100) {  // 类型数据大小限制
		*error = ERROR_PARAMETER_SIZE;
		return false;
	}

	// 初始化消息结构体
	MSGSTRUCT msg{};
	msg.head.id = CREATEITEM;
	msg.head.recsize = tagsize;
	msg.head.bodysize = typesize;

	// 安全拷贝字符串（防止缓冲区溢出）
	strncpy(msg.head.qname, "BOARD", sizeof(msg.head.qname) - 1);
	msg.head.qname[sizeof(msg.head.qname) - 1] = '\0';

	strncpy(msg.head.itemname, tagname, sizeof(msg.head.itemname) - 1);
	msg.head.itemname[sizeof(msg.head.itemname) - 1] = '\0';

	// 发送消息头
	if (send_all(sockfd, &msg, sizeof(MSGHEAD)) <= 0) {
		*error = errno;
		close(sockfd);
		return false;
	}

	// 发送类型数据
	if (send_all(sockfd, type, typesize) <= 0) {
		*error = errno;
		close(sockfd);
		return false;
	}

	// 读取响应
	if (readn(sockfd, &msg, sizeof(MSGHEAD)) < 0) {
		*error = errno;
		close(sockfd);
		return false;
	}

	// 验证响应
	if (msg.head.bodysize > 0) {
		*error = ERROR_INVALID_RESPONSE;
		close(sockfd);
		return false;
	}

	*error = msg.head.error;
	return (*error == 0);
}

extern "C" bool deletetag(int sockfd, const char* tagname, unsigned int* error)
{
	// 参数校验
	if (!tagname || !error) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}
	// 初始化消息结构体
	MSGSTRUCT msg{};
	msg.head.id = DELETEITEM;
	msg.head.bodysize = 0;
	// 安全拷贝字符串（防止缓冲区溢出）
	strncpy(msg.head.qname, "BOARD", sizeof(msg.head.qname) - 1);
	msg.head.qname[sizeof(msg.head.qname) - 1] = '\0';
	strncpy(msg.head.itemname, tagname, sizeof(msg.head.itemname) - 1);
	msg.head.itemname[sizeof(msg.head.itemname) - 1] = '\0';
	// 发送请求
	if (send_all(sockfd, &msg, sizeof(MSGHEAD)) <= 0) {
		*error = errno;
		close(sockfd);
		return false;
	}
	// 读取响应
	if (readn(sockfd, &msg, sizeof(MSGHEAD)) < 0) {
		*error = errno;
		close(sockfd);
		return false;
	}
	// 验证响应
	if (msg.head.bodysize > 0) {
		*error = ERROR_INVALID_RESPONSE;
		close(sockfd);
		return false;
	}
	*error = msg.head.error;
	return (*error == 0);
}

extern "C" bool waitpostdata(int sockfd, std::string& tagname, void* value, int buffersize, int timeout, unsigned int* error)
{
	//绝对不能在本地实现超时，否则容易出现问题
	MSGSTRUCT msg{};
	msg.head.id = POSTWAIT;
	msg.head.timeout = timeout;  // 或 htonl(timeout)
	msg.head.bodysize = 0;

	if (send_all(sockfd, &msg, sizeof(MSGHEAD)) <= 0) {
		*error = errno;
		close(sockfd);
		return false;
	}

	if (readn(sockfd, &msg, sizeof(MSGHEAD)) < 0) {
		*error = errno;
		close(sockfd);
		return false;
	}

	// 验证数据大小
	if (msg.head.bodysize > buffersize) {
		*error = ERROR_BUFFER_TOO_SMALL;
		close(sockfd);
		return false;
	}

	// 读取数据体（如果有）
	if (msg.head.bodysize > 0) {
		if (readn(sockfd, value, msg.head.bodysize) < 0) {
			*error = errno;
			close(sockfd);
			return false;
		}
	}

	// 确保字符串以 null 结尾
	if (msg.head.bodysize < buffersize) {
		((char*)value)[msg.head.bodysize] = '\0';  // 确保 null 结尾
	}
	else {
		((char*)value)[buffersize - 1] = '\0';  // 防止溢出
	}

	*error = msg.head.error;  // 或 ntohl(msg.head.error)
	if (*error != 0) {
		tagname = (*error == ETIMEDOUT) ? "WAIT_TIMEOUT" : "";
		return (*error == ETIMEDOUT);  // 仅超时返回 true
	}

	tagname = msg.head.itemname ? msg.head.itemname : "";
	return true;
}

extern "C" bool createqueue(int sockfd, const char* queuename, int recordsize, int recordnum, int operatemode, void* type, int typesize, unsigned int* error)
{
	// 参数校验
	if (recordsize <= 0 || recordnum <= 0 || type == nullptr || typesize <= 0) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}

	if (recordsize > 2048 || typesize > 100)
	{
		errorCode = ERROR_PARAMETER_SIZE;
		return false;
	}

	// 初始化消息结构体
	MSGSTRUCT msg{};
	msg.head.id = CREATEQUEUE;
	msg.head.recsize = recordsize;
	msg.head.count = recordnum;
	msg.head.start = operatemode;
	msg.head.bodysize = typesize;

	// 安全拷贝字符串（防止缓冲区溢出）
	strncpy(msg.head.qname, queuename, sizeof(msg.head.qname) - 1);
	msg.head.qname[sizeof(msg.head.qname) - 1] = '\0';

	// 发送消息头
	if (send_all(sockfd, &msg, sizeof(MSGHEAD)) <= 0) {
		*error = errno;
		close(sockfd);
		return false;
	}

	// 发送类型数据
	if (send_all(sockfd, type, typesize) <= 0) {
		*error = errno;
		close(sockfd);
		return false;
	}

	// 读取响应
	if (readn(sockfd, &msg, sizeof(MSGHEAD)) < 0) {
		*error = errno;
		close(sockfd);
		return false;
	}

	// 验证响应
	if (msg.head.bodysize > 0) {
		*error = ERROR_INVALID_RESPONSE;
		close(sockfd);
		return false;
	}

	*error = msg.head.error;
	return (*error == 0);
}

/*F+F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F
Function: CreateQ

Summary:  Create queue file according the specified size and tye.

Args:     const char*  lpFileName
queue/file name
int recordSize
record size
int recordNum
total number of record in queue
int dataType
1 ASCII, 0 BINARY
int operateMode
1 shift queue, 0 general queue

Returns:  bool
F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F-F*/
extern "C" bool CreateQ(const char* lpFileName,
	int recordSize,
	int recordNum,
	int dateType,
	int operateMode,
	void* pType,
	int typeSize)
{
	if (recordSize > MAXMSGLEN || typeSize > TYPEMAXSIZE)
	{
		errorCode = ERROR_PARAMETER_SIZE;
		return false;
	}

	// 检查文件名长度是否超过定义长度
	if (strlen(lpFileName) > MAXDQNAMELENTH)
	{
		errorCode = ERROR_FILENAME_TOO_LONG;
		return false;
	}

	// 形成文件路径全名
	char dqFileName[100];
	strcpy(dqFileName, dataQuePath);
	strcat(dqFileName, lpFileName);

	// 先查找同名文件删除之
	if (fs::exists(dqFileName))
	{
		if (!fs::remove(dqFileName)) {		//C++17
			errorCode = ERROR_FILE_IN_USE;
			return false;
		}
	}

	// 创建父目录（如果不存在）
	fs::path path(dqFileName);
	if (path.has_parent_path() && !fs::exists(path.parent_path())) {
		if (!fs::create_directories(path.parent_path())) {
			errorCode = ERROR_FILE_CREATE_FAILSURE;
			return false;
		}
	}

	// 计算文件大小
	long fileSize;
	fileSize = recordNum * (recordSize + RECORDHEADSIZE) + QUEUEHEADSIZE + TYPEMAXSIZE;

	// 1. 创建或打开文件（O_CREAT | O_RDWR 表示不存在则创建，存在则打开可读写）
	int fd = open(dqFileName, O_CREAT | O_RDWR, 0644);
	if (fd == -1) {
		std::cerr << "打开/创建文件失败: " << strerror(errno) << std::endl;
		return false;
	}

	// 2. 将文件扩展至目标大小
	if (ftruncate(fd, fileSize) == -1) {
		std::cerr << "调整文件大小失败: " << strerror(errno) << std::endl;
		close(fd);
		return false;
	}

	// 3. 内存映射文件
	void* lpMapAddress = mmap(nullptr, fileSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (lpMapAddress == MAP_FAILED) {
		std::cerr << "内存映射失败: " << strerror(errno) << std::endl;
		close(fd);
		return false;
	}

	// 4. 使用映射内存（示例：填充零）
	memset(lpMapAddress, 0, fileSize);

	QUEUE_HEAD* pDqHead;
	pDqHead = (QUEUE_HEAD*)lpMapAddress;
	pDqHead->qbdtype = QUEUE_T;
	pDqHead->dataType = dateType;
	pDqHead->operateMode = operateMode;
	pDqHead->num = recordNum;
	pDqHead->size = recordSize;
	pDqHead->writePoint = recordNum - 1;
	pDqHead->readPoint = (operateMode == NORMAL_MODE) ? (recordNum - 1) : 0;
	gettime(pDqHead->createDate);
	if (pType != 0 && typeSize != 0 && typeSize < TYPEMAXSIZE)
	{
		memcpy((char*)lpMapAddress + recordNum * (recordSize + RECORDHEADSIZE) + QUEUEHEADSIZE,
			pType,
			typeSize
		);
		pDqHead->typesize = typeSize;
	}
	else
	{
		pDqHead->typesize = 0;
	}
	//ZeroMemory((char*)lpMapAddress + QUEUEHEADSIZE, recordNum * (recordSize + RECORDHEADSIZE));	//初始化数据区
	memset((char*)lpMapAddress + QUEUEHEADSIZE, 0, recordNum * (recordSize + RECORDHEADSIZE));
	RECORD_HEAD* pRecordHead;
	for (int i = 0; i < recordNum; i++)
	{
		pRecordHead = (RECORD_HEAD*)((char*)lpMapAddress + QUEUEHEADSIZE + i * (recordSize + RECORDHEADSIZE));
		pRecordHead->ack = 0;
		pRecordHead->index = i;
	}

	// 5. 同步数据到磁盘（可选）
	if (msync(lpMapAddress, fileSize, MS_SYNC) == -1) {
		std::cerr << "同步到磁盘失败: " << strerror(errno) << std::endl;
	}

	// 6. 解除映射并关闭文件
	munmap(lpMapAddress, fileSize);
	close(fd);

	return true;
}

/*F+F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F
Function: LoadQ

Summary:  Close mutex, file-mapping object and file.

Args:     const char*  lpDqName

Returns:  bool
F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F-F*/
extern "C" bool LoadQ(const char* lpDqName)
{
	usereason = 1;

	//这里就不锁定了，因为LoadQ肯定在单线程上运行，和OpenQ不一样，另外下面的是fetchtab，函数里也要锁定的，这样会死锁的
	//std::unique_lock<std::mutex> lock(mutex_rw);

	struct TABLE_MSG tabmsg;
	if (fetchtab(lpDqName, tabmsg))
	{
		errorCode = ERROR_ALREADY_LOAD;
		return true;
	}

	// 形成文件全名
	char dqFileName[100];
	strcpy(dqFileName, dataQuePath);
	strcat(dqFileName, lpDqName);

	// 打开文件
	int fd = open(dqFileName, O_CREAT | O_RDWR, 0644);
	if (fd == -1) {
		std::cerr << "打开文件失败: " << strerror(errno) << std::endl;
		return false;
	}

	// 2. 获取文件大小
	struct stat sb;
	if (fstat(fd, &sb) == -1) {
		std::cerr << "获取文件大小失败: " << strerror(errno) << std::endl;
		close(fd);
		return false;
	}
	off_t file_size = sb.st_size;

	// 3. 创建内存映射
	void* lpMapAddress = mmap(nullptr, file_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (lpMapAddress == MAP_FAILED) {
		std::cerr << "内存映射失败: " << strerror(errno) << std::endl;
		close(fd);
		return false;
	}

	//创建读写锁
	if (*(int*)lpMapAddress == BOARD_T)
	{
		BOARD_HEAD* pHead = (BOARD_HEAD*)lpMapAddress;
		std::mutex* pSharedMutex = new (&pHead->mutex_rw) std::mutex();
		tabmsg.pmutex_rw = pSharedMutex;
		for (int i = 0; i < MUTEXSIZE; i++)
		{
			pSharedMutex = new (&pHead->mutex_rw_tag[i]) std::mutex();
		}
	}
	else if (*(int*)lpMapAddress == DATABASE_T)
	{
		DB_HEAD* pHead = (DB_HEAD*)lpMapAddress;
		std::mutex* pSharedMutex = new (&pHead->mutex_rw) std::mutex();
		tabmsg.pmutex_rw = pSharedMutex;
	}
	else if (*(int*)lpMapAddress == QUEUE_T)
	{
		tabmsg.pmutex_rw = nullptr;
	}

	//将队列名、文件句柄、映射文件对象句柄、互斥量对象句柄保存在哈希表中。
	strcpy(tabmsg.dqname, lpDqName);
	tabmsg.hFile = fd;
	tabmsg.hMapFile = 0;				  //linux下不需要
	tabmsg.lpMapAddress = lpMapAddress;   //为将内存文件映射数据定期写回文件而增加
	//tabmsg.hMutex = hMutex;
	pthread_mutex_init(&tabmsg.hMutex, NULL);
	tabmsg.filesize = file_size;		  //linux下新增
	if (inserttab(tabmsg))
	{
		return true;
	}
	//CloseHandle(hMapFile);
	close(fd);
	pthread_mutex_destroy(&tabmsg.hMutex);
	return false;
}

/*F+F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F
Function: UnloadQ

Summary:  Close mutex, file-mapping object and file.

Args:     const char*  lpDqName

Returns:  BOOL
F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F-F*/
extern "C" bool UnloadQ(const char* lpDqName)
{
	//从哈希表中查找该数据队列。
	struct TABLE_MSG tabmsg;
	if (deletetab(lpDqName, tabmsg))
	{
		//关闭映射文件对象句柄、互斥量对象句柄、文件句柄。
		//CloseHandle(tabmsg.hMapFile);
		close(tabmsg.hFile);
		pthread_mutex_destroy(&tabmsg.hMutex);

		//关闭读写锁
		if (tabmsg.pmutex_rw != nullptr)
		{
			tabmsg.pmutex_rw->~mutex();
			tabmsg.pmutex_rw = nullptr;
		}

		if (*(int*)tabmsg.lpMapAddress == BOARD_T)
		{
			BOARD_HEAD* pHead = (BOARD_HEAD*)tabmsg.lpMapAddress;
			for (int i = 0; i < MUTEXSIZE; i++)
			{
				pHead->mutex_rw_tag[i].~mutex();
			}
		}
	}
	return true;
}

/*F+F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F
Function: UnloadQ

Summary:  Unload all queue and bulletin loaded by this process.

Args:

Returns:  BOOL
F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F-F*/
extern "C" bool UnloadAll(void)
{
	for (int i = 0; i < TABLESIZE; i++)
	{
		if (strcmp(table[i].dqname, "\0") && !table[i].erased)
		{
			//CloseHandle(table[i].hMapFile);
			close(table[i].hFile);
			pthread_mutex_destroy(&table[i].hMutex);
			strcpy(table[i].dqname, "\0");

			//关闭读写锁
			if (table[i].pmutex_rw != nullptr)
			{
				table[i].pmutex_rw->~mutex();
				table[i].pmutex_rw = nullptr;
			}
		}
	}
	return true;
}


//为将内存文件映射数据定期写回文件而增加
/*F+F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F
Function: FlushQFile

Summary:  将内存文件映射数据写回文件.

Args:

Returns:  BOOL
F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F-F*/
extern "C" bool FlushQFile(void)
{
	for (int i = 0; i < TABLESIZE; i++)
	{
		if (strcmp(table[i].dqname, "\0") && !table[i].erased && table[i].lpMapAddress != NULL)
		{
			if (msync(table[i].lpMapAddress, table[i].filesize, MS_SYNC) == -1)
			{
				std::cerr << table[i].dqname << " msync failed: " << strerror(errno) << std::endl;
			}
		}
	}
	return true;
}

/*F+F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F
Function: ReadHead

Summary:  Read queue head.

Args:     const char*  lpDqName
void  *lpHead
pointer of head buffer

Returns:  BOOL
F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F-F*/
extern "C" bool ReadHead(const char* lpDqName, void* lpHead)
{
	// 从哈希表中查找该数据队列。
	struct TABLE_MSG tabmsg;
	if (!fetchtab(lpDqName, tabmsg))
	{
		return false;
	}

	// 根据映射内存地址、互斥量对象句柄从数据队列取出队列头。
	//WaitForSingleObject(tabmsg.hMutex, INFINITE);
	pthread_mutex_lock(&tabmsg.hMutex);
	memcpy(lpHead, tabmsg.lpMapAddress, QUEUEHEADSIZE);
	//ReleaseMutex(tabmsg.hMutex);
	pthread_mutex_unlock(&tabmsg.hMutex);
	return true;
}

/*F+F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F
Function: ReadQ

Summary:  Read a record from queue.

Args:     const char*  lpDqName
void  *lpRecord
pointer of record buffer
int actSize
size of record

Returns:  BOOL
F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F-F*/
extern "C" bool ReadQ(const char* lpDqName, void* lpRecord, int actSize, char* remoteIp)
{
	// 从哈希表中查找该数据队列。
	struct TABLE_MSG tabmsg;
	if (!fetchtab(lpDqName, tabmsg))
	{
		return false;
	}
	void* lpMapAddress;
	pthread_mutex_t hMutex;
	lpMapAddress = tabmsg.lpMapAddress;
	hMutex = tabmsg.hMutex;
	QUEUE_HEAD* pDqHead;
	pDqHead = (QUEUE_HEAD*)lpMapAddress;
	int num, size;
	num = pDqHead->num;
	size = pDqHead->size;

	// 验证记录长度是否相符。
	if (((pDqHead->dataType == ASCII_TYPE) && (actSize < size))
		|| ((pDqHead->dataType == BINARY_TYPE) && (actSize != size)))
	{
		errorCode = ERROR_RECORDSIZE;
		return false;
	}

	// 根据映射内存地址、互斥量对象句柄从数据队列取出一条记录。
	//WaitForSingleObject(hMutex, INFINITE);
	pthread_mutex_lock(&hMutex);
	if (pDqHead->operateMode == NORMAL_MODE)
	{
		if (pDqHead->readPoint != pDqHead->writePoint)
		{
			pDqHead->readPoint = (pDqHead->readPoint + 1) % num;

			RECORD_HEAD* pRecordHead = (RECORD_HEAD*)((char*)lpMapAddress + (pDqHead->readPoint) * (size + RECORDHEADSIZE) + QUEUEHEADSIZE);
			if (remoteIp != 0)
			{
				strcpy(remoteIp, pRecordHead->remoteIp);
			}

			memcpy(lpRecord, (char*)pRecordHead + RECORDHEADSIZE, size);
			//ReleaseMutex(hMutex);
			pthread_mutex_unlock(&hMutex);
			return true;
		}
	}
	else
	{
		if (pDqHead->readPoint != 0)
		{
			RECORD_HEAD* pRecordHead = (RECORD_HEAD*)((char*)lpMapAddress + (pDqHead->writePoint) * (size + RECORDHEADSIZE) + QUEUEHEADSIZE);
			if (remoteIp != 0)
			{
				strcpy(remoteIp, pRecordHead->remoteIp);
			}

			memcpy(lpRecord, (char*)pRecordHead + RECORDHEADSIZE, size);
			//ReleaseMutex(hMutex);
			pthread_mutex_unlock(&hMutex);
			return true;
		}
	}

	//ReleaseMutex(hMutex);
	pthread_mutex_unlock(&hMutex);
	errorCode = ERROR_DQ_EMPTY;
	return false;
}

//从队列中删除一条记录（读指针加1），不返回数据
extern "C" bool PopJustRecordFromQueue(const char* lpDqName)
{
	// 从哈希表中查找该数据队列。
	struct TABLE_MSG tabmsg;
	if (!fetchtab(lpDqName, tabmsg))
	{
		return false;
	}
	void* lpMapAddress;
	pthread_mutex_t hMutex;
	lpMapAddress = tabmsg.lpMapAddress;
	hMutex = tabmsg.hMutex;
	QUEUE_HEAD* pDqHead;
	pDqHead = (QUEUE_HEAD*)lpMapAddress;
	int num = pDqHead->num;

	// 根据映射内存地址、互斥量对象句柄从数据队列弹出一条记录。
	//WaitForSingleObject(hMutex, INFINITE);
	pthread_mutex_lock(&hMutex);
	if (pDqHead->operateMode == NORMAL_MODE)
	{
		if (pDqHead->readPoint != pDqHead->writePoint)
		{
			pDqHead->readPoint = (pDqHead->readPoint + 1) % num;
			//ReleaseMutex(hMutex);
			pthread_mutex_unlock(&hMutex);
			return true;
		}
		else
		{
			//ReleaseMutex(hMutex);
			pthread_mutex_unlock(&hMutex);
			errorCode = ERROR_DQ_EMPTY;
			return false;
		}
	}
	//ReleaseMutex(hMutex);
	pthread_mutex_unlock(&hMutex);
	errorCode = 0;
	return false;
}

/*F+F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F
Function: MulReadQ

Summary:  Read multiple records from queue.

Args:     const char*  lpDqName
void  *lpRecord
pointer of record buffer
int start
start position
int * count
actual read count

Returns:  BOOL
F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F-F*/
extern "C" bool MulReadQ(const char* lpDqName, void* lpRecord, int start, int* count, int* pRecordSize)
{
	// 从哈希表中查找该数据队列。
	struct TABLE_MSG tabmsg;
	if (!fetchtab(lpDqName, tabmsg))
	{
		return false;
	}
	void* lpMapAddress;
	pthread_mutex_t hMutex;
	lpMapAddress = tabmsg.lpMapAddress;
	hMutex = tabmsg.hMutex;

	QUEUE_HEAD* pDqHead;
	pDqHead = (QUEUE_HEAD*)lpMapAddress;
	int num, size;
	num = pDqHead->num;
	size = pDqHead->size;
	if (pRecordSize != 0)
	{
		*pRecordSize = size;
	}
	if (start > (num - 1))
	{
		errorCode = ERROR_STARTPOSITION;
		return false;
	}
	int readCount;
	if (pDqHead->operateMode == NORMAL_MODE)
	{
		readCount = (num < *count) ? num : *count;
		*count = readCount;
		for (int i = 0; i < readCount; i++)
		{
			memcpy((char*)lpRecord + i * (size + RECORDHEADSIZE),
				(char*)lpMapAddress + ((start + i) % num) * (size + RECORDHEADSIZE) + QUEUEHEADSIZE,
				size + RECORDHEADSIZE);
		}
		return true;
	}
	else	// SHIFT_MODE
	{
		readCount = ((num - start) < *count) ? (num - start) : *count;
		*count = readCount;
		int actstart;
		actstart = (pDqHead->writePoint + num - start) % num;
		for (int i = 0; i < readCount; i++)
		{
			memcpy((char*)lpRecord + i * (size + RECORDHEADSIZE),
				(char*)lpMapAddress + ((actstart + num - i) % num) * (size + RECORDHEADSIZE) + QUEUEHEADSIZE,
				size + RECORDHEADSIZE);
		}
		return true;
	}
}

extern "C" bool MulReadQ2(const char* lpDqName, void** lppRecords, int start, int* count, int* pRecordSize)
{
	// 从哈希表中查找该数据队列。
	struct TABLE_MSG tabmsg;
	if (!fetchtab(lpDqName, tabmsg))
	{
		return false;
	}
	void* lpMapAddress;
	pthread_mutex_t hMutex;
	lpMapAddress = tabmsg.lpMapAddress;
	hMutex = tabmsg.hMutex;

	QUEUE_HEAD* pDqHead;
	pDqHead = (QUEUE_HEAD*)lpMapAddress;
	int num, size;
	num = pDqHead->num;
	size = pDqHead->size;
	if (pRecordSize != 0)
	{
		*pRecordSize = size;
	}
	if (start > (num - 1))
	{
		errorCode = ERROR_STARTPOSITION;
		*lppRecords = 0;
		*count = 0;
		*pRecordSize = 0;
		return false;
	}
	unsigned long memsize;
	void* pBuff;
	int readCount;
	//WaitForSingleObject(hMutex, INFINITE);
	pthread_mutex_lock(&hMutex);
	if (pDqHead->operateMode == NORMAL_MODE)
	{
		readCount = (num < *count) ? num : *count;
		*count = readCount;

		memsize = readCount * (size + RECORDHEADSIZE);
		pBuff = malloc(memsize);
		if (pBuff == NULL)
		{
			*lppRecords = 0;
			*count = 0;
			//ReleaseMutex(hMutex);
			pthread_mutex_unlock(&hMutex);
			return false;
		}
		for (int i = 0; i < readCount; i++)
		{
			memcpy((char*)pBuff + i * (size + RECORDHEADSIZE),
				(char*)lpMapAddress + ((start + i) % num) * (size + RECORDHEADSIZE) + QUEUEHEADSIZE,
				size + RECORDHEADSIZE);
		}
		//ReleaseMutex(hMutex);
		pthread_mutex_unlock(&hMutex);
		*lppRecords = pBuff;
		return true;
	}
	else	// SHIFT_MODE
	{
		readCount = ((num - start) < *count) ? (num - start) : *count;
		*count = readCount;

		memsize = readCount * (size + RECORDHEADSIZE);
		pBuff = malloc(memsize);
		if (pBuff == NULL)
		{
			*lppRecords = 0;
			*count = 0;
			//ReleaseMutex(hMutex);
			pthread_mutex_unlock(&hMutex);
			return false;
		}
		int actstart;
		actstart = (pDqHead->writePoint + num - start) % num;
		for (int i = 0; i < readCount; i++)
		{
			memcpy((char*)pBuff + i * (size + RECORDHEADSIZE),
				(char*)lpMapAddress + ((actstart + num - i) % num) * (size + RECORDHEADSIZE) + QUEUEHEADSIZE,
				size + RECORDHEADSIZE);
		}
		//ReleaseMutex(hMutex);
		pthread_mutex_unlock(&hMutex);
		*lppRecords = pBuff;
		return true;
	}
}

/*F+F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F
Function: WriteQ

Summary:  Write a record to queue.

Args:     const char*  lpDqName
void  *lpRecord
pointer of record buffer
int actSize
size of record

Returns:  BOOL
F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F-F*/
extern "C" bool WriteQ(const char* lpDqName, void* lpRecord, int actSize, const char* remoteIp)
{
	// 从哈希表中查找该数据队列。
	struct TABLE_MSG tabmsg;
	if (!fetchtab(lpDqName, tabmsg))
	{
		return false;
	}
	void* lpMapAddress;
	pthread_mutex_t hMutex;
	lpMapAddress = tabmsg.lpMapAddress;
	hMutex = tabmsg.hMutex;
	QUEUE_HEAD* pDqHead;
	pDqHead = (QUEUE_HEAD*)lpMapAddress;
	int num, size;
	num = pDqHead->num;
	size = pDqHead->size;

	// 字符串型且实际长度缺省，用字符串长度加1设定实际长度
	if ((pDqHead->dataType == ASCII_TYPE) && (actSize == 0))
	{
		actSize = (int)(strlen((const char*)lpRecord) + 1);	//mark x64	如果是UNICODE字符串怎么办
	}

	// 验证记录长度是否相符。
	if (((pDqHead->dataType == ASCII_TYPE) && (actSize > size))
		|| ((pDqHead->dataType == BINARY_TYPE) && (actSize != size)))
	{
		errorCode = ERROR_RECORDSIZE;
		return false;
	}

	// 根据映射内存地址、互斥量对象句柄将一条记录写入数据队列。
	//WaitForSingleObject(hMutex, INFINITE);
	pthread_mutex_lock(&hMutex);
	if (pDqHead->operateMode == NORMAL_MODE)
	{
		if ((pDqHead->writePoint + 1) % num == pDqHead->readPoint)
		{
			//ReleaseMutex(hMutex);
			pthread_mutex_unlock(&hMutex);
			errorCode = ERROR_DQ_FULL;
			return false;
		}
	}
	pDqHead->writePoint = (pDqHead->writePoint + 1) % num;
	char* lpwriteAddress;
	lpwriteAddress = (char*)lpMapAddress + (pDqHead->writePoint) * (size + RECORDHEADSIZE) + QUEUEHEADSIZE;	//x64
	gettime((char*)lpwriteAddress);
	RECORD_HEAD* pRecordHead = (RECORD_HEAD*)lpwriteAddress;
	pRecordHead->ack = 0;
	if (remoteIp == 0)
	{
		strcpy(pRecordHead->remoteIp, "127.0.0.1");
	}
	else
	{
		strcpy(pRecordHead->remoteIp, remoteIp);
	}
	memcpy(lpwriteAddress + RECORDHEADSIZE, lpRecord, actSize);
	if (pDqHead->operateMode == SHIFT_MODE)
	{
		pDqHead->readPoint = 1;
	}
	//ReleaseMutex(hMutex);
	pthread_mutex_unlock(&hMutex);
	return true;
}

/*F+F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F
Function: ClearQ

Summary:  Clear queue.

Args:     const char*  lpDqName

Returns:  BOOL
F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F-F*/
extern "C" bool ClearQ(const char* lpDqName)
{
	// 从哈希表中查找该数据队列。
	struct TABLE_MSG tabmsg;
	if (!fetchtab(lpDqName, tabmsg))
	{
		return false;
	}
	void* lpMapAddress;
	pthread_mutex_t hMutex;
	lpMapAddress = tabmsg.lpMapAddress;
	hMutex = tabmsg.hMutex;

	// 清空数据队列
	QUEUE_HEAD* pDqHead;
	pDqHead = (QUEUE_HEAD*)lpMapAddress;
	//WaitForSingleObject(hMutex, INFINITE);
	pthread_mutex_lock(&hMutex);
	if (pDqHead->operateMode == NORMAL_MODE)
	{
		pDqHead->readPoint = pDqHead->writePoint = pDqHead->num - 1;
	}
	else
	{
		pDqHead->readPoint = 0;
	}
	//ReleaseMutex(hMutex);
	pthread_mutex_unlock(&hMutex);
	return false;
}

/*F+F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F
Function: SetPtrQ

Summary:  Set read and write pointer of queue.

Args:     const char*  lpDqName, int readPtr, int writePtr

Returns:  BOOL
F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F-F*/
extern "C" bool SetPtrQ(const char* lpDqName, int readPtr, int writePtr)
{
	// 从哈希表中查找该数据队列。
	struct TABLE_MSG tabmsg;
	if (!fetchtab(lpDqName, tabmsg))
	{
		return false;
	}
	void* lpMapAddress;
	pthread_mutex_t hMutex;
	lpMapAddress = tabmsg.lpMapAddress;
	hMutex = tabmsg.hMutex;
	QUEUE_HEAD* pDqHead;
	pDqHead = (QUEUE_HEAD*)lpMapAddress;
	int num;
	num = pDqHead->num;

	if (readPtr >= 0 && writePtr >= 0 && readPtr < num && writePtr < num)
	{
		//WaitForSingleObject(hMutex, INFINITE);
		pthread_mutex_lock(&hMutex);
		pDqHead->readPoint = readPtr;
		pDqHead->writePoint = writePtr;
		//ReleaseMutex(hMutex);
		pthread_mutex_unlock(&hMutex);
		return true;
	}

	if (readPtr >= 0 && readPtr < num && writePtr == -1)
	{
		//WaitForSingleObject(hMutex, INFINITE);
		pthread_mutex_lock(&hMutex);
		pDqHead->readPoint = readPtr;
		//ReleaseMutex(hMutex);
		pthread_mutex_unlock(&hMutex);
		return true;
	}

	if (writePtr >= 0 && writePtr < num && readPtr == -1)
	{
		//WaitForSingleObject(hMutex, INFINITE);
		pthread_mutex_lock(&hMutex);
		pDqHead->writePoint = writePtr;
		//ReleaseMutex(hMutex);
		pthread_mutex_unlock(&hMutex);
		return true;
	}

	return false;
}


/*F+F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F
Function: PeekQ

Summary:  Read a record but not remove it from queue.

Args:     const char*  lpDqName
void  *lpRecord
pointer of record buffer
int actSize
size of record

Returns:  BOOL
F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F-F*/
extern "C" bool PeekQ(const char* lpDqName, void* lpRecord, int actSize)
{
	// 从哈希表中查找该数据队列。
	struct TABLE_MSG tabmsg;
	if (!fetchtab(lpDqName, tabmsg))
	{
		return false;
	}
	void* lpMapAddress;
	pthread_mutex_t hMutex;
	lpMapAddress = tabmsg.lpMapAddress;
	hMutex = tabmsg.hMutex;

	// 根据映射内存地址、互斥量对象句柄从数据队列取出一条记录。
	QUEUE_HEAD* pDqHead;
	pDqHead = (QUEUE_HEAD*)lpMapAddress;
	int num, size;
	num = pDqHead->num;
	size = pDqHead->size;

	// 验证记录长度是否相符。
	if (((pDqHead->dataType == ASCII_TYPE) && (actSize < size))
		|| ((pDqHead->dataType == BINARY_TYPE) && (actSize != size)))
	{
		errorCode = ERROR_RECORDSIZE;
		return false;
	}

	// 本函数不支持移位队列。
	if (pDqHead->operateMode == SHIFT_MODE)
	{
		errorCode = ERROR_OPERATE_PROHIBIT;
		return false;
	}

	//WaitForSingleObject(hMutex, INFINITE);
	pthread_mutex_lock(&hMutex);
	if (pDqHead->readPoint != pDqHead->writePoint)
	{
		memcpy(lpRecord, (char*)lpMapAddress + ((pDqHead->readPoint + 1) % num) * (size + RECORDHEADSIZE) + QUEUEHEADSIZE + RECORDHEADSIZE, size);
		//ReleaseMutex(hMutex);
		pthread_mutex_unlock(&hMutex);
		return true;
	}
	//ReleaseMutex(hMutex);
	pthread_mutex_unlock(&hMutex);
	errorCode = ERROR_DQ_EMPTY;
	return false;
}


/*F+F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F
Function: IsEmptyQ

Summary:  Judge whether queue is empty.

Args:     const char*  lpDqName

Returns:  BOOL
F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F-F*/
extern "C" bool IsEmptyQ(const char* lpDqName)
{
	// 从哈希表中查找该数据队列。
	struct TABLE_MSG tabmsg;
	if (!fetchtab(lpDqName, tabmsg))
	{
		return false;
	}
	void* lpMapAddress;
	pthread_mutex_t hMutex;
	lpMapAddress = tabmsg.lpMapAddress;
	hMutex = tabmsg.hMutex;

	// 根据映射内存地址、互斥量对象句柄从数据队列取出一条记录。
	QUEUE_HEAD* pDqHead;
	pDqHead = (QUEUE_HEAD*)lpMapAddress;
	//WaitForSingleObject(hMutex, INFINITE);
	pthread_mutex_lock(&hMutex);
	if (pDqHead->operateMode == NORMAL_MODE)
	{
		if (pDqHead->readPoint != pDqHead->writePoint)
		{
			//ReleaseMutex(hMutex);
			pthread_mutex_unlock(&hMutex);
			return false;
		}
	}
	else
	{
		if (pDqHead->readPoint != 0)
		{
			//ReleaseMutex(hMutex);
			pthread_mutex_unlock(&hMutex);
			return false;
		}
	}
	//ReleaseMutex(hMutex);
	pthread_mutex_unlock(&hMutex);
	return true;
}


/*F+F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F
Function: IsFullQ

Summary:  Judge whether queue is full.

Args:     const char*  lpDqName

Returns:  BOOL
F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F-F*/
extern "C" bool IsFullQ(const char* lpDqName)
{
	// 从哈希表中查找该数据队列。
	struct TABLE_MSG tabmsg;
	if (!fetchtab(lpDqName, tabmsg))
	{
		return false;
	}
	void* lpMapAddress;
	pthread_mutex_t hMutex;
	lpMapAddress = tabmsg.lpMapAddress;
	hMutex = tabmsg.hMutex;

	// 根据映射内存地址、互斥量对象句柄从数据队列取出一条记录。
	QUEUE_HEAD* pDqHead;
	int num;
	pDqHead = (QUEUE_HEAD*)lpMapAddress;
	num = pDqHead->num;
	//WaitForSingleObject(hMutex, INFINITE);
	pthread_mutex_lock(&hMutex);
	if (pDqHead->operateMode == NORMAL_MODE)
	{
		if ((pDqHead->writePoint + 1) % num != pDqHead->readPoint)
		{
			//ReleaseMutex(hMutex);
			pthread_mutex_unlock(&hMutex);
			return false;
		}
	}
	else
	{
		//ReleaseMutex(hMutex);
		pthread_mutex_unlock(&hMutex);
		return false;
	}
	//ReleaseMutex(hMutex);
	pthread_mutex_unlock(&hMutex);
	return true;
}

/*F+F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F
Function: CreateB

Summary:  Create bulletin file according the specified size.

Args:     const char*  lpFileName
bulletin/file name
int size
bulletin data size, the file size includes bulletin data size
and head size.

Returns:  BOOL
F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F-F*/
extern "C" bool CreateB(const char* lpFileName, int size)
{
	usereason = 0;

	// 检查文件名长度是否超过定义长度
	if (strlen(lpFileName) > MAXDQNAMELENTH)
	{
		errorCode = ERROR_FILENAME_TOO_LONG;
		return false;
	}

	// 形成文件路径全名
	char dqFileName[100];
	strcpy(dqFileName, dataQuePath);
	strcat(dqFileName, lpFileName);

	// 先查找同名文件删除之
	if (fs::exists(dqFileName))
	{
		if (!fs::remove(dqFileName)) {		//C++17
			errorCode = ERROR_FILE_IN_USE;
			return false;
		}
	}

	// 创建父目录（如果不存在）
	fs::path path(dqFileName);
	if (path.has_parent_path() && !fs::exists(path.parent_path())) {
		if (!fs::create_directories(path.parent_path())) {
			errorCode = ERROR_FILE_CREATE_FAILSURE;
			return false;
		}
	}

	// 计算文件大小
	int fileSize;
	//fileSize = sizeof(BOARD_HEAD) + size + INDEXSIZE * TYPEMAXSIZE;
	fileSize = sizeof(BOARD_HEAD) + size + INDEXSIZE * TYPEAVGSIZE;	//mark

	// 1. 创建或打开文件（O_CREAT | O_RDWR 表示不存在则创建，存在则打开可读写）
	int fd = open(dqFileName, O_CREAT | O_RDWR, 0644);
	if (fd == -1) {
		std::cerr << "打开/创建文件失败: " << strerror(errno) << std::endl;
		return false;
	}

	// 2. 将文件扩展至目标大小
	if (ftruncate(fd, fileSize) == -1) {
		std::cerr << "调整文件大小失败: " << strerror(errno) << std::endl;
		close(fd);
		return false;
	}

	// 3. 内存映射文件
	void* lpMapAddress = mmap(nullptr, fileSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (lpMapAddress == MAP_FAILED) {
		std::cerr << "内存映射失败: " << strerror(errno) << std::endl;
		close(fd);
		return false;
	}

	// 初始化公告板头和数据区
	memset(lpMapAddress, 0, fileSize);
	BOARD_HEAD* pHead;
	pHead = (BOARD_HEAD*)lpMapAddress;
	pHead->qbdtype = BOARD_T;
	pHead->totalsize = sizeof(BOARD_HEAD) + size;	// BOARD_HEAD和后面数据区大小的和，不包括最后面的类型区
	pHead->typesize = INDEXSIZE * TYPEAVGSIZE;		// 最后面的类型区的大小 mark
	pHead->remain = size;
	pHead->typeremain = INDEXSIZE * TYPEAVGSIZE;	// mark
	pHead->counter = 0;
	pHead->nextpos = 0;
	pHead->nexttypepos = 0;
	pHead->indexcount = 0;

	// 5. 同步数据到磁盘（可选）
	if (msync(lpMapAddress, fileSize, MS_SYNC) == -1) {
		std::cerr << "同步到磁盘失败: " << strerror(errno) << std::endl;
	}

	// 6. 解除映射并关闭文件
	munmap(lpMapAddress, fileSize);
	close(fd);

	return true;
}

/*F+F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F
Function: ReadInfoB

Summary:  Read bulletin information.

Args:     const char* lpBulletinName
bulletin name
BOARD_HEAD * pHead
pointer of head buffer

Returns:  BOOL
F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F-F*/
extern "C" bool ReadInfoB(const char* lpBulletinName,
	int* pTotalSize,
	int* pDataSize,
	int* pLeftSize,
	int* pItemNum,
	int* buffSize,
	char ppBuff[][24])
{
	// Search bulletin in hash table.
	struct TABLE_MSG tabmsg;
	if (!fetchtab(lpBulletinName, tabmsg))
	{
		return false;
	}

	BOARD_HEAD* pHead;
	pHead = (BOARD_HEAD*)tabmsg.lpMapAddress;

	// 根据映射内存地址、互斥量对象句柄从公告板取出公告板头。
	//WaitForSingleObject(tabmsg.hMutex, INFINITE);
	std::lock_guard<std::mutex> lock(*tabmsg.pmutex_rw);
	if (pTotalSize != 0)
	{
		*pTotalSize = pHead->totalsize;
	}
	if (pDataSize != 0)
	{
		*pDataSize = pHead->totalsize - sizeof(BOARD_HEAD);
	}
	if (pLeftSize != 0)
	{
		*pLeftSize = pHead->remain;
	}
	if (pItemNum != 0)
	{
		*pItemNum = pHead->indexcount;
	}

	if (buffSize != 0)
	{
		if (pHead->indexcount > *buffSize)
		{
			*buffSize = 0;
		}
		else if (ppBuff != NULL)
		{
			*buffSize = pHead->indexcount;
			int index = 0;
			for (int i = 0; i < INDEXSIZE; i++)
			{
				//				if (index>=pHead->indexcount) break;
				if (pHead->index[i].itemname[0] != '\0' && pHead->index[i].erased == false)
				{
					strcpy(ppBuff[index], pHead->index[i].itemname);
					index++;
				}
			}
			if (index != pHead->indexcount)
				return false;
		}
	}

	//ReleaseMutex(tabmsg.hMutex);
	return true;
}

/*F+F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F
Function: ReadB

Summary:  Read the specified item from bulletin.

Args:     const char* lpBulletinName
bulletin name
const char* lpItemName
item name to be read
void  *lpItem
item buffer pointer
int actSize
item size

Returns:  BOOL
F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F-F*/
extern "C" bool ReadB(const char* lpBulletinName, const char* lpItemName, void* lpItem, int actSize, timespec* timestamp)
{
	// Search bulletin in hash table.
	struct TABLE_MSG tabmsg;
	if (!fetchtab(lpBulletinName, tabmsg))
	{
		return false;
	}
	void* lpMapAddress;
	pthread_mutex_t hMutex;
	lpMapAddress = tabmsg.lpMapAddress;
	hMutex = tabmsg.hMutex;
	BOARD_HEAD* pHead;
	BOARD_INDEX_STRUCT* pIndex;
	pHead = (BOARD_HEAD*)lpMapAddress;
	pIndex = pHead->index;

	// search item in bulletin's index(hash table)
	int loc, c;
	loc = hash1(lpItemName);
	c = hash2(lpItemName);

	//std::lock_guard<std::mutex> lock(*tabmsg.pmutex_rw);
	tabmsg.pmutex_rw->lock();

	while (strcmp(pIndex[loc].itemname, "\0") && strcmp(pIndex[loc].itemname, lpItemName))
	{
		loc = (loc + c) % INDEXSIZE;
	}

	if (!strcmp(pIndex[loc].itemname, "\0") || pIndex[loc].erased)
	{
		// item not found
		errorCode = ERROR_RECORD_NOT_EXIST;
		tabmsg.pmutex_rw->unlock();
		return false;
	}

	// validate item size
	if (pIndex[loc].itemsize != actSize)
	{
		errorCode = ERROR_RECORDSIZE;
		tabmsg.pmutex_rw->unlock();
		return false;
	}

	tabmsg.pmutex_rw->unlock();

	std::lock_guard<std::mutex> lock(pHead->mutex_rw_tag[loc & (MUTEXSIZE - 1)]);

	memcpy(lpItem, (char*)lpMapAddress + sizeof(BOARD_HEAD) + pIndex[loc].startpos, actSize);
	if (timestamp != 0)
	{
		*timestamp = pIndex[loc].timestamp;
	}

	return true;
}

extern "C" int GetStringLength(const char* lpBulletinName, const char* lpItemName)
{
	// Search bulletin in hash table.
	struct TABLE_MSG tabmsg;
	if (!fetchtab(lpBulletinName, tabmsg))
	{
		return -1;
	}

	BOARD_HEAD* pHead = static_cast<BOARD_HEAD*>(tabmsg.lpMapAddress);
	BOARD_INDEX_STRUCT* pIndex = pHead->index;

	// search item in bulletin's index(hash table)
	int loc, c;
	loc = hash1(lpItemName);
	c = hash2(lpItemName);
	tabmsg.pmutex_rw->lock();
	while (pIndex[loc].itemname[0] != '\0' && strcmp(pIndex[loc].itemname, lpItemName))
	{
		loc = (loc + c) % INDEXSIZE;
	}
	if (!strcmp(pIndex[loc].itemname, "\0") || pIndex[loc].erased)
	{
		// item not found
		tabmsg.pmutex_rw->unlock();
		return -1;
	}
	tabmsg.pmutex_rw->unlock();
	return pIndex[loc].strlenth; // 返回实际字符串长度，不包括'\0'结尾
}

extern "C" bool ReadB_String(const char* lpBulletinName, const char* lpItemName, void* lpItem, int actSize, timespec* timestamp)
{
	// Search bulletin in hash table.
	struct TABLE_MSG tabmsg;
	if (!fetchtab(lpBulletinName, tabmsg))
	{
		return false;
	}
	void* lpMapAddress;
	pthread_mutex_t hMutex;
	lpMapAddress = tabmsg.lpMapAddress;
	hMutex = tabmsg.hMutex;
	BOARD_HEAD* pHead;
	BOARD_INDEX_STRUCT* pIndex;
	pHead = (BOARD_HEAD*)lpMapAddress;
	pIndex = pHead->index;

	// search item in bulletin's index(hash table)
	int loc, c;
	loc = hash1(lpItemName);
	c = hash2(lpItemName);

	//std::lock_guard<std::mutex> lock(*tabmsg.pmutex_rw);
	tabmsg.pmutex_rw->lock();

	while (strcmp(pIndex[loc].itemname, "\0") && strcmp(pIndex[loc].itemname, lpItemName))
	{
		loc = (loc + c) % INDEXSIZE;
	}

	if (!strcmp(pIndex[loc].itemname, "\0") || pIndex[loc].erased)
	{
		// item not found
		errorCode = ERROR_RECORD_NOT_EXIST;
		tabmsg.pmutex_rw->unlock();
		return false;
	}

	// validate item size
	//if (pIndex[loc].itemsize > actSize)
	//改为验证实际字符串长度是否超过缓冲区大小
	if (pIndex[loc].strlenth > actSize)
	{
		errorCode = BUFFER_TOO_SMALL;
		tabmsg.pmutex_rw->unlock();
		return false;
	}

	tabmsg.pmutex_rw->unlock();

	std::lock_guard<std::mutex> lock(pHead->mutex_rw_tag[loc & (MUTEXSIZE - 1)]);

	//ZeroMemory(lpItem, actSize);
	//mark 确保返回的字符串是以'\0'结尾的，但当缓冲区长度刚好等于实际字符串长度的时候，不能保证'\0'结尾
	memset(lpItem, 0, actSize);
	//memcpy(lpItem, (char*)lpMapAddress + sizeof(BOARD_HEAD) + pIndex[loc].startpos, pIndex[loc].itemsize);
	//mark 改为拷贝实际长度
	memcpy(lpItem, (char*)lpMapAddress + sizeof(BOARD_HEAD) + pIndex[loc].startpos, pIndex[loc].strlenth);
	if (timestamp != 0)
	{
		*timestamp = pIndex[loc].timestamp;
	}

	return true;
}

//返回字符串的实际长度，不包括'\0'结尾	
extern "C" bool ReadB_String2(const char* lpBulletinName, const char* lpItemName, void* lpItem, int actSize, int& strLength, timespec* timestamp)
{
	// Search bulletin in hash table.
	struct TABLE_MSG tabmsg;
	if (!fetchtab(lpBulletinName, tabmsg))
	{
		return false;
	}
	void* lpMapAddress;
	pthread_mutex_t hMutex;
	lpMapAddress = tabmsg.lpMapAddress;
	hMutex = tabmsg.hMutex;
	BOARD_HEAD* pHead;
	BOARD_INDEX_STRUCT* pIndex;
	pHead = (BOARD_HEAD*)lpMapAddress;
	pIndex = pHead->index;

	// search item in bulletin's index(hash table)
	int loc, c;
	loc = hash1(lpItemName);
	c = hash2(lpItemName);

	//std::lock_guard<std::mutex> lock(*tabmsg.pmutex_rw);
	tabmsg.pmutex_rw->lock();

	while (strcmp(pIndex[loc].itemname, "\0") && strcmp(pIndex[loc].itemname, lpItemName))
	{
		loc = (loc + c) % INDEXSIZE;
	}

	if (!strcmp(pIndex[loc].itemname, "\0") || pIndex[loc].erased)
	{
		// item not found
		errorCode = ERROR_RECORD_NOT_EXIST;
		tabmsg.pmutex_rw->unlock();
		return false;
	}

	// validate item size
	//if (pIndex[loc].itemsize > actSize)
	//改为验证实际字符串长度是否超过缓冲区大小
	if (pIndex[loc].strlenth > actSize)
	{
		errorCode = BUFFER_TOO_SMALL;
		tabmsg.pmutex_rw->unlock();
		return false;
	}

	tabmsg.pmutex_rw->unlock();

	std::lock_guard<std::mutex> lock(pHead->mutex_rw_tag[loc & (MUTEXSIZE - 1)]);

	//ZeroMemory(lpItem, actSize);
	//mark 确保返回的字符串是以'\0'结尾的，但当缓冲区长度刚好等于实际字符串长度的时候，不能保证'\0'结尾
	//注释是因为返回了实际长度，不需要'\0'结尾，由调用方决定是否添加'\0'结尾
	//memset(lpItem, 0, actSize);
	//memcpy(lpItem, (char*)lpMapAddress + sizeof(BOARD_HEAD) + pIndex[loc].startpos, pIndex[loc].itemsize);
	//mark 改为拷贝实际长度
	memcpy(lpItem, (char*)lpMapAddress + sizeof(BOARD_HEAD) + pIndex[loc].startpos, pIndex[loc].strlenth);
	if (timestamp != 0)
	{
		*timestamp = pIndex[loc].timestamp;
	}

	strLength = pIndex[loc].strlenth; // 返回实际字符串长度，不包括'\0'结尾	
	return true;
}

/*F+F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F
Function: WriteB

Summary:  Write the specified item into bulletin.

Args:     const char* lpBulletinName
bulletin name
const char* lpItemName
item name to be writen
void  *lpItem
item buffer pointer
int actSize
item size
int offset
item offset

Returns:  BOOL
F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F-F*/
extern "C" bool WriteB(const char* lpBulletinName, const char* lpItemName, void* lpItem, int actSize, void* lpSubItem, int actSubSize)
{
	// Search bulletin in hash table.
	struct TABLE_MSG tabmsg;
	if (!fetchtab(lpBulletinName, tabmsg))
	{
		return false;
	}
	void* lpMapAddress;
	pthread_mutex_t hMutex;
	lpMapAddress = tabmsg.lpMapAddress;
	hMutex = tabmsg.hMutex;
	BOARD_HEAD* pHead;
	BOARD_INDEX_STRUCT* pIndex;
	pHead = (BOARD_HEAD*)lpMapAddress;
	pIndex = pHead->index;

	// search item in bulletin's index(hash table)
	int loc, c;
	loc = hash1(lpItemName);
	c = hash2(lpItemName);

	////std::unique_lock<std::mutex> lock(*tabmsg.pmutex_rw);
	tabmsg.pmutex_rw->lock();

	while (strcmp(pIndex[loc].itemname, "\0") && strcmp(pIndex[loc].itemname, lpItemName))
	{
		loc = (loc + c) % INDEXSIZE;
	}

	if (!strcmp(pIndex[loc].itemname, "\0") || pIndex[loc].erased)
	{
		// item not found
		errorCode = ERROR_RECORD_NOT_EXIST;
		tabmsg.pmutex_rw->unlock();
		return false;

		// 不再自动创建
		// item not found,create it
		/*
		// no enough remained space
		if (pHead->remain < actSize)
		{
			errorCode = ERROR_NO_SPACE;	//mark
			tabmsg.pmutex_rw->unlock();
			return false;
		}

		// index is full
		if (pHead->indexcount == INDEXSIZE - 1)
		{
			errorCode = ERROR_TABLE_OVERFLOW;
			tabmsg.pmutex_rw->unlock();
			return false;
		}

		tabmsg.pmutex_rw->unlock();

		//创建前需锁定后重新搜索可用地址mark
		tabmsg.pmutex_rw->lock();

		int loc, c;
		loc = hash1(lpItemName);
		c = hash2(lpItemName);
		while (strcmp(pIndex[loc].itemname, "\0") && pIndex[loc].erased == false)
		{
			loc = (loc + c) % INDEXSIZE;
		}

		// create item index
		pHead->indexcount++;
		strcpy(pIndex[loc].itemname, lpItemName);
		pIndex[loc].itemsize = actSize;
		pIndex[loc].startpos = pHead->nextpos;
		pIndex[loc].erased = false;	//mark
		pHead->nextpos += actSize;
		pHead->remain -= actSize;

		tabmsg.pmutex_rw->unlock();
		*/
	}
	else
	{
		tabmsg.pmutex_rw->unlock();
	}

	std::unique_lock<std::mutex> lock(pHead->mutex_rw_tag[loc & (MUTEXSIZE - 1)]);

	if (lpSubItem == 0)
	{
		// validate item size
		if (pIndex[loc].itemsize != actSize)
		{
			errorCode = ERROR_RECORDSIZE;
			return false;
		}
		memcpy((char*)lpMapAddress + sizeof(BOARD_HEAD) + pIndex[loc].startpos, lpItem, actSize);
		//_time64(&pIndex[loc].timestamp);
		clock_gettime(CLOCK_REALTIME, &pIndex[loc].timestamp);
	}
	else
	{
		// validate item size
		if (pIndex[loc].itemsize != actSize || (char*)lpSubItem - (char*)lpItem + actSubSize > actSize)
		{
			errorCode = ERROR_RECORDSIZE;
			return false;
		}
		memcpy((char*)lpMapAddress + sizeof(BOARD_HEAD) + pIndex[loc].startpos + ((char*)lpSubItem - (char*)lpItem), lpSubItem, actSubSize);
		//_time64(&pIndex[loc].timestamp);
		clock_gettime(CLOCK_REALTIME, &pIndex[loc].timestamp);
	}
	//pHead->counter++;	//mark 如果打开，意味着tabmsg.pmutex_rw要一直写锁定或再次写锁定，意义不大
	return true;
}

extern "C" bool WriteB_String(const char* lpBulletinName, const char* lpItemName, void* lpItem, int actSize, void* lpSubItem, int actSubSize)
{
	// Search bulletin in hash table.
	struct TABLE_MSG tabmsg;
	if (!fetchtab(lpBulletinName, tabmsg))
	{
		return false;
	}
	void* lpMapAddress;
	pthread_mutex_t hMutex;
	lpMapAddress = tabmsg.lpMapAddress;
	hMutex = tabmsg.hMutex;
	BOARD_HEAD* pHead;
	BOARD_INDEX_STRUCT* pIndex;
	pHead = (BOARD_HEAD*)lpMapAddress;
	pIndex = pHead->index;

	// search item in bulletin's index(hash table)
	int loc, c;
	loc = hash1(lpItemName);
	c = hash2(lpItemName);

	//std::unique_lock<std::mutex> lock(*tabmsg.pmutex_rw);
	tabmsg.pmutex_rw->lock();

	while (strcmp(pIndex[loc].itemname, "\0") && strcmp(pIndex[loc].itemname, lpItemName))
	{
		loc = (loc + c) % INDEXSIZE;
	}

	if (!strcmp(pIndex[loc].itemname, "\0") || pIndex[loc].erased)	// item not found,create it
	{
		// item not found
		errorCode = ERROR_RECORD_NOT_EXIST;
		tabmsg.pmutex_rw->unlock();
		return false;
	}
	else
	{
		tabmsg.pmutex_rw->unlock();
	}

	std::unique_lock<std::mutex> lock(pHead->mutex_rw_tag[loc & (MUTEXSIZE - 1)]);

	if (lpSubItem == 0)
	{
		// validate item size
		if (pIndex[loc].itemsize < actSize)
		{
			errorCode = STRING_TOO_LONG;
			return false;
		}

		pIndex[loc].strlenth = actSize;	//mark 记录字符串长度，便于读取时判断是否足够
		//ZeroMemory((char*)lpMapAddress + sizeof(BOARD_HEAD) + pIndex[loc].startpos, pIndex[loc].itemsize);
		//mark 下面的置零操作现在有些多余，因为字段strlenth已经记录了当前字符串的长度，但为了安全，还是保留了
		memset((char*)lpMapAddress + sizeof(BOARD_HEAD) + pIndex[loc].startpos, 0, pIndex[loc].itemsize);
		memcpy((char*)lpMapAddress + sizeof(BOARD_HEAD) + pIndex[loc].startpos, lpItem, actSize);
		//_time64(&pIndex[loc].timestamp);
		clock_gettime(CLOCK_REALTIME, &pIndex[loc].timestamp);
	}
	else
	{
		//不支持
		return false;
	}
	//pHead->counter++;	//mark 如果打开，意味着tabmsg.pmutex_rw要一直写锁定或再次写锁定，意义不大
	return true;
}

/*F+F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F
Function: WriteBOffSet

Summary:  Write the specified item into bulletin.

Args:     const char* lpBulletinName
bulletin name
const char* lpItemName
item name to be writen
void  *lpItem
item buffer pointer
int actSize
item size
int offset
item offset

Returns:  BOOL
F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F-F*/
extern "C" bool WriteBOffSet(const char* lpBulletinName, const char* lpItemName, void* lpItem, int actSize, int offSet, int actSubSize)
{
	return WriteB(lpBulletinName, lpItemName, lpItem, actSize, (char*)lpItem + offSet, actSubSize);
}

extern "C" bool ClearB(const char* lpBoardName)
{
	// 从哈希表中查找该数据队列。
	struct TABLE_MSG tabmsg;
	if (!fetchtab(lpBoardName, tabmsg))
	{
		return false;
	}

	void* lpMapAddress;
	lpMapAddress = tabmsg.lpMapAddress;
	BOARD_HEAD* pHead;
	pHead = (BOARD_HEAD*)lpMapAddress;

	if (pHead->qbdtype != BOARD_T) return false;

	std::unique_lock<std::mutex> lock(*tabmsg.pmutex_rw);

	int totalsize = pHead->totalsize;
	int typesize = pHead->typesize;
	int fileSize = pHead->totalsize + pHead->typesize;
	//ZeroMemory((char*)pHead->index, sizeof(pHead->index));
	memset((char*)pHead->index, 0, sizeof(pHead->index));
	pHead->qbdtype = BOARD_T;
	pHead->totalsize = totalsize;	// BOARD_HEAD和后面数据区大小的和，不包括最后面的类型区
	pHead->typesize = typesize;		// 最后面的类型区的大小 mark
	pHead->remain = totalsize - sizeof(BOARD_HEAD);
	pHead->typeremain = typesize;
	pHead->counter = 0;
	pHead->nextpos = 0;
	pHead->nexttypepos = 0;
	pHead->indexcount = 0;

	return true;
}

/*F+F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F
Function: DeleteItem

Summary:  Delete Item.

Args:     const char* lpDBName
database name
const char* lpTableName
table name

Returns:  BOOL
F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F-F*/
extern "C" bool DeleteItem(const char* lpBoardName, const char* lpItemName)
{
	// 从哈希表中查找该数据队列。
	struct TABLE_MSG tabmsg;
	if (!fetchtab(lpBoardName, tabmsg))
	{
		return false;
	}

	void* lpMapAddress;
	pthread_mutex_t hMutex;
	lpMapAddress = tabmsg.lpMapAddress;
	hMutex = tabmsg.hMutex;
	BOARD_HEAD* pHead;
	BOARD_INDEX_STRUCT* pIndex;
	pHead = (BOARD_HEAD*)lpMapAddress;
	pIndex = pHead->index;

	//WaitForSingleObject(hMutex, INFINITE);
	std::unique_lock<std::mutex> lock(*tabmsg.pmutex_rw);

	// search table in DB's index(hash table)
	int loc, c;
	loc = hash1(lpItemName);
	c = hash2(lpItemName);
	while (strcmp(pIndex[loc].itemname, "\0") && strcmp(pIndex[loc].itemname, lpItemName))
	{
		loc = (loc + c) % INDEXSIZE;
	}

	if (!strcmp(pIndex[loc].itemname, "\0") || pIndex[loc].erased)
	{
		// table not found
		errorCode = ERROR_ITEM_NOT_EXIST;
		//ReleaseMutex(hMutex);
		return false;
	}

	strcpy(pIndex[loc].itemname, "CT0D9GG_~$59");	 //不是必需的，只是确保安全
	pIndex[loc].erased = true;
	//pIndex[loc].typesize = 0; mark

	int pos = pIndex[loc].startpos;
	int itemsize = pIndex[loc].itemsize;

	void* Destination = (char*)lpMapAddress + sizeof(BOARD_HEAD) + pos;
	const void* Source = (char*)Destination + itemsize;
	int Length = pHead->totalsize - sizeof(BOARD_HEAD) - pos - itemsize;
	//memmove(Destination, Source, Length);
	memmove(Destination, Source, Length);

	//mark
	int typepos = pIndex[loc].typeaddr;
	int typesize = pIndex[loc].typesize;
	pIndex[loc].typesize = 0;
	Destination = (char*)lpMapAddress + pHead->totalsize + typepos;
	Source = (char*)Destination + typesize;
	Length = INDEXSIZE * TYPEAVGSIZE - typepos - typesize;
	//memmove(Destination, Source, Length);
	memmove(Destination, Source, Length);

	for (int i = 0; i < INDEXSIZE; i++)
	{
		//if (strcmp(pIndex[i].itemname, "\0") && !pIndex[i].erased && pIndex[i].startpos > pos)
		//{
		//	pIndex[i].startpos -= itemsize;
		//}

		//mark
		if (strcmp(pIndex[i].itemname, "\0") && !pIndex[i].erased)
		{
			if (pIndex[i].startpos > pos)
				pIndex[i].startpos -= itemsize;

			if (pIndex[i].typeaddr > typepos)
				pIndex[i].typeaddr -= typesize;
		}
	}

	pHead->nextpos -= itemsize;
	pHead->remain += itemsize;
	pHead->indexcount--;

	//mark
	pHead->nexttypepos -= typesize;
	pHead->typeremain += typesize;

	//ReleaseMutex(hMutex);
	return true;
}

/*F+F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F
Function: CreateTable

Summary:  Create table in DB.

Args:     const char* lpDBName
database name
const char* lpItemName
item name to be writen
void  *lpItem
item buffer pointer
int actSize
item size
int offset
item offset

Returns:  BOOL
F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F-F*/
extern "C" bool CreateTable(const char* lpDBName, const char* lpTableName, int recordSize, int maxCount, void* pType, int typeSize)
{
	if (recordSize > MAXMSGLEN || typeSize > TYPEMAXSIZE)
	{
		errorCode = ERROR_PARAMETER_SIZE;
		return false;
	}

	// Search bulletin in hash table.
	struct TABLE_MSG tabmsg;
	if (!fetchtab(lpDBName, tabmsg))
	{
		return false;
	}
	void* lpMapAddress;
	pthread_mutex_t hMutex;
	lpMapAddress = tabmsg.lpMapAddress;
	hMutex = tabmsg.hMutex;
	DB_HEAD* pHead;
	DB_INDEX_STRUCT* pIndex;
	pHead = (DB_HEAD*)lpMapAddress;
	pIndex = pHead->index;

	//WaitForSingleObject(hMutex, INFINITE);
	std::unique_lock<std::mutex> lock(*tabmsg.pmutex_rw);

	// search table in DB's index(hash table)
	int loc, c, loc_s, c_s;
	loc = hash1(lpTableName);
	c = hash2(lpTableName);
	loc_s = loc, c_s = c;
	while (strcmp(pIndex[loc].tablename, "\0") && strcmp(pIndex[loc].tablename, lpTableName))
	{
		loc = (loc + c) % INDEXSIZE;
	}

	if (strcmp(pIndex[loc].tablename, lpTableName) == 0 && pIndex[loc].erased == false)
	{
		errorCode = ERROR_TABLE_ALREADY_EXIST;
		//ReleaseMutex(hMutex);
		return false;
	}

	int totalsize;
	totalsize = recordSize * maxCount;

	// no enough remained space
	if (pHead->remain < totalsize)
	{
		errorCode = ERROR_NO_SPACE;
		//ReleaseMutex(hMutex);
		return false;
	}

	loc = loc_s;
	c = c_s;
	while (strcmp(pIndex[loc].tablename, "\0") && !pIndex[loc].erased)
	{
		loc = (loc + c) % INDEXSIZE;
	}

	if (pHead->indexcount == INDEXSIZE - 1)
	{
		errorCode = ERROR_TABLE_OVERFLOW;
		//ReleaseMutex(hMutex);
		return false;
	}

	pHead->indexcount++;
	strcpy(pIndex[loc].tablename, lpTableName);
	pIndex[loc].recordsize = recordSize;
	pIndex[loc].maxcount = maxCount;
	pIndex[loc].currcount = 0;
	pIndex[loc].startpos = pHead->nextpos;
	pIndex[loc].erased = false;
	pHead->nextpos += totalsize;
	pHead->remain -= totalsize;
	//_time64(&pIndex[loc].timestamp);
	clock_gettime(CLOCK_REALTIME, &pIndex[loc].timestamp);
	//if (pType != 0 && typeSize != 0 && typeSize < TYPEMAXSIZE)
	if (pType != 0 && typeSize != 0 && typeSize < TYPEMAXSIZE && typeSize < pHead->typeremain)
	{
		//CopyMemory((char*)lpMapAddress + pHead->totalsize + loc * TYPEMAXSIZE,
		//	pType,
		//	typeSize
		//);
		//pIndex[loc].typesize = typeSize;

		//mark
		memcpy((char*)lpMapAddress + pHead->totalsize + pHead->nexttypepos,
			pType,
			typeSize
		);
		pIndex[loc].typesize = typeSize;
		pIndex[loc].typeaddr = pHead->nexttypepos;
		pHead->nexttypepos += typeSize;
		pHead->typeremain -= typeSize;
	}
	else
	{
		pIndex[loc].typesize = 0;
	}
	//ReleaseMutex(hMutex);
	return true;
}

/*F+F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F
Function: ReadHead

Summary:  Read queue head.

Args:     const char*  lpDqName
void  *lpHeadstatic char   buffer[
pointer of head buffer

Returns:  BOOL
F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F-F*/
extern "C" bool ReadHeadDB(const char* lpDBName, void* lpHead)
{
	// 从哈希表中查找该数据队列。
	struct TABLE_MSG tabmsg;
	if (!fetchtab(lpDBName, tabmsg))
	{
		return false;
	}

	// 根据映射内存地址、互斥量对象句柄从数据队列取出队列头。
	//WaitForSingleObject(tabmsg.hMutex, INFINITE);
	std::lock_guard<std::mutex> lock(*tabmsg.pmutex_rw);
	memcpy(lpHead, tabmsg.lpMapAddress, sizeof(DB_HEAD));
	//ReleaseMutex(tabmsg.hMutex);
	return true;
}

/*F+F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F
Function: ClearTable

Summary:  Clear table.

Args:     const char* lpDBName
database name
const char* lpTableName
table name

Returns:  BOOL
F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F-F*/
extern "C" bool ClearTable(const char* lpDBName, const char* lpTableName)
{
	// 从哈希表中查找该数据队列。
	struct TABLE_MSG tabmsg;
	if (!fetchtab(lpDBName, tabmsg))
	{
		return false;
	}

	void* lpMapAddress;
	pthread_mutex_t hMutex;
	lpMapAddress = tabmsg.lpMapAddress;
	hMutex = tabmsg.hMutex;
	DB_HEAD* pHead;
	DB_INDEX_STRUCT* pIndex;
	pHead = (DB_HEAD*)lpMapAddress;
	pIndex = pHead->index;

	//WaitForSingleObject(hMutex, INFINITE);
	std::unique_lock<std::mutex> lock(*tabmsg.pmutex_rw);

	// search table in bulletin's index(hash table)
	int loc, c;
	loc = hash1(lpTableName);
	c = hash2(lpTableName);
	while (strcmp(pIndex[loc].tablename, "\0") && strcmp(pIndex[loc].tablename, lpTableName))
	{
		loc = (loc + c) % INDEXSIZE;
	}

	if (!strcmp(pIndex[loc].tablename, "\0") || pIndex[loc].erased)
	{
		// table not found
		errorCode = ERROR_TABLE_NOT_EXIST;
		//ReleaseMutex(hMutex);
		return false;
	}

	// clear table
	pIndex[loc].currcount = 0;
	//_time64(&pIndex[loc].timestamp);
	clock_gettime(CLOCK_REALTIME, &pIndex[loc].timestamp);

	pHead->counter++;
	//ReleaseMutex(hMutex);
	return true;
}

/*F+F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F
Function: UpdateTable

Summary:  Update Table.

Args:     const char* lpDBName
database name
const char* lpTableName
table name

Returns:  BOOL
F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F-F*/
extern "C" bool UpdateTable(const char* lpDBName, const char* lpTableName, int rowid, int recordSize, void* lpRecord)
{
	if (rowid < 0 || recordSize == 0) return true;

	// 从哈希表中查找该数据队列。
	struct TABLE_MSG tabmsg;
	if (!fetchtab(lpDBName, tabmsg))
	{
		return false;
	}

	void* lpMapAddress;
	pthread_mutex_t hMutex;
	lpMapAddress = tabmsg.lpMapAddress;
	hMutex = tabmsg.hMutex;
	DB_HEAD* pHead;
	DB_INDEX_STRUCT* pIndex;
	pHead = (DB_HEAD*)lpMapAddress;
	pIndex = pHead->index;

	//WaitForSingleObject(hMutex, INFINITE);
	std::unique_lock<std::mutex> lock(*tabmsg.pmutex_rw);

	// search table in DB's index(hash table)
	int loc, c;
	loc = hash1(lpTableName);
	c = hash2(lpTableName);
	while (strcmp(pIndex[loc].tablename, "\0") && strcmp(pIndex[loc].tablename, lpTableName))
	{
		loc = (loc + c) % INDEXSIZE;
	}

	if (!strcmp(pIndex[loc].tablename, "\0") || pIndex[loc].erased)
	{
		// table not found
		errorCode = ERROR_TABLE_NOT_EXIST;
		//ReleaseMutex(hMutex);
		return false;
	}

	if (recordSize != pIndex[loc].recordsize)
	{
		errorCode = ERROR_RECORDSIZE;
		//ReleaseMutex(hMutex);
		return false;
	}

	if (!(rowid < pIndex[loc].currcount))
	{
		errorCode = ERROR_TABLE_ROWID;
		//ReleaseMutex(hMutex);
		return false;
	}

	memcpy((char*)lpMapAddress + sizeof(DB_HEAD) + pIndex[loc].startpos + recordSize * rowid, lpRecord, recordSize);

	pHead->counter++;
	//ReleaseMutex(hMutex);
	return true;
}

/*F+F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F
Function: InsertTable

Summary:  Insert Table.

Args:     const char* lpDBName
database name
const char* lpTableName
table name

Returns:  BOOL
F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F-F*/
extern "C" bool InsertTable(const char* lpDBName, const char* lpTableName, int count, int recordSize, void* lpRecords)
{
	if (count == 0 || recordSize == 0) return true;

	// 从哈希表中查找该数据队列。
	struct TABLE_MSG tabmsg;
	if (!fetchtab(lpDBName, tabmsg))
	{
		return false;
	}

	void* lpMapAddress;
	pthread_mutex_t hMutex;
	lpMapAddress = tabmsg.lpMapAddress;
	hMutex = tabmsg.hMutex;
	DB_HEAD* pHead;
	DB_INDEX_STRUCT* pIndex;
	pHead = (DB_HEAD*)lpMapAddress;
	pIndex = pHead->index;

	//WaitForSingleObject(hMutex, INFINITE);
	std::unique_lock<std::mutex> lock(*tabmsg.pmutex_rw);

	// search table in DB's index(hash table)
	int loc, c;
	loc = hash1(lpTableName);
	c = hash2(lpTableName);
	while (strcmp(pIndex[loc].tablename, "\0") && strcmp(pIndex[loc].tablename, lpTableName))
	{
		loc = (loc + c) % INDEXSIZE;
	}

	if (!strcmp(pIndex[loc].tablename, "\0") || pIndex[loc].erased)
	{
		// table not found
		errorCode = ERROR_TABLE_NOT_EXIST;
		//ReleaseMutex(hMutex);
		return false;
	}

	if (recordSize != pIndex[loc].recordsize)
	{
		errorCode = ERROR_RECORDSIZE;
		//ReleaseMutex(hMutex);
		return false;
	}

	if (pIndex[loc].currcount + count > pIndex[loc].maxcount)
	{
		errorCode = ERROR_NO_SPACE;
		//ReleaseMutex(hMutex);
		return false;
	}

	memcpy((char*)lpMapAddress + sizeof(DB_HEAD) + pIndex[loc].startpos + recordSize * pIndex[loc].currcount, lpRecords, recordSize * count);
	pIndex[loc].currcount += count;
	//_time64(&pIndex[loc].timestamp);
	clock_gettime(CLOCK_REALTIME, &pIndex[loc].timestamp);

	pHead->counter++;
	//ReleaseMutex(hMutex);
	return true;
}


extern "C" bool RefreshTable(const char* lpDBName, const char* lpTableName, int count, int recordSize, void* lpRecords)
{
	if (count == 0 || recordSize == 0) return true;

	// 从哈希表中查找该数据队列。
	struct TABLE_MSG tabmsg;
	if (!fetchtab(lpDBName, tabmsg))
	{
		return false;
	}

	void* lpMapAddress;
	pthread_mutex_t hMutex;
	lpMapAddress = tabmsg.lpMapAddress;
	hMutex = tabmsg.hMutex;
	DB_HEAD* pHead;
	DB_INDEX_STRUCT* pIndex;
	pHead = (DB_HEAD*)lpMapAddress;
	pIndex = pHead->index;

	//WaitForSingleObject(hMutex, INFINITE);
	std::unique_lock<std::mutex> lock(*tabmsg.pmutex_rw);

	// search table in DB's index(hash table)
	int loc, c;
	loc = hash1(lpTableName);
	c = hash2(lpTableName);
	while (strcmp(pIndex[loc].tablename, "\0") && strcmp(pIndex[loc].tablename, lpTableName))
	{
		loc = (loc + c) % INDEXSIZE;
	}

	if (!strcmp(pIndex[loc].tablename, "\0") || pIndex[loc].erased)
	{
		// table not found
		errorCode = ERROR_TABLE_NOT_EXIST;
		//ReleaseMutex(hMutex);
		return false;
	}

	if (recordSize != pIndex[loc].recordsize)
	{
		errorCode = ERROR_RECORDSIZE;
		//ReleaseMutex(hMutex);
		return false;
	}

	// clear table
	pIndex[loc].currcount = 0;

	if (pIndex[loc].currcount + count > pIndex[loc].maxcount)
	{
		errorCode = ERROR_NO_SPACE;
		//ReleaseMutex(hMutex);
		return false;
	}

	memcpy((char*)lpMapAddress + sizeof(DB_HEAD) + pIndex[loc].startpos + recordSize * pIndex[loc].currcount, lpRecords, recordSize * count);
	pIndex[loc].currcount += count;
	//_time64(&pIndex[loc].timestamp);
	clock_gettime(CLOCK_REALTIME, &pIndex[loc].timestamp);

	pHead->counter++;
	//ReleaseMutex(hMutex);
	return true;
}

/*F+F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F
Function: SelectTable

Summary:  Select Table.

Args:     const char* lpDBName
database name
const char* lpTableName
table name

Returns:  BOOL
F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F-F*/
extern "C" bool SelectTable(const char* lpDBName, const char* lpTableName, void** lppRecords, int* pCount, int* pRecordSize, timespec* pWriteTime)
{
	// 从哈希表中查找该数据队列。
	struct TABLE_MSG tabmsg;
	if (!fetchtab(lpDBName, tabmsg))
	{
		return false;
	}

	void* lpMapAddress;
	pthread_mutex_t hMutex;
	lpMapAddress = tabmsg.lpMapAddress;
	hMutex = tabmsg.hMutex;
	DB_HEAD* pHead;
	DB_INDEX_STRUCT* pIndex;
	pHead = (DB_HEAD*)lpMapAddress;
	pIndex = pHead->index;

	//WaitForSingleObject(hMutex, INFINITE);
	std::lock_guard<std::mutex> lock(*tabmsg.pmutex_rw);

	// search table in DB's index(hash table)
	int loc, c;
	loc = hash1(lpTableName);
	c = hash2(lpTableName);
	while (strcmp(pIndex[loc].tablename, "\0") && strcmp(pIndex[loc].tablename, lpTableName))
	{
		loc = (loc + c) % INDEXSIZE;
	}

	if (!strcmp(pIndex[loc].tablename, "\0") || pIndex[loc].erased)
	{
		// table not found
		errorCode = ERROR_TABLE_NOT_EXIST;
		*lppRecords = 0;
		*pCount = 0;
		*pRecordSize = 0;
		//ReleaseMutex(hMutex);
		return false;
	}

	if (pIndex[loc].currcount == 0)
	{
		*lppRecords = 0;
		*pCount = 0;
		*pRecordSize = pIndex[loc].recordsize;
		if (pWriteTime != 0)
		{
			*pWriteTime = pIndex[loc].timestamp;
		}
		//ReleaseMutex(hMutex);
		return true;
	}

	int memsize;
	void* pBuff;

	memsize = pIndex[loc].recordsize * pIndex[loc].currcount;
	pBuff = malloc(memsize);
	if (pBuff == NULL)
	{
		*lppRecords = 0;
		*pCount = 0;
		*pRecordSize = 0;
		//ReleaseMutex(hMutex);
		return false;
	}

	memcpy(pBuff, (char*)lpMapAddress + sizeof(DB_HEAD) + pIndex[loc].startpos, memsize);
	*lppRecords = pBuff;
	*pCount = pIndex[loc].currcount;
	*pRecordSize = pIndex[loc].recordsize;
	if (pWriteTime != 0)
	{
		*pWriteTime = pIndex[loc].timestamp;
	}

	//ReleaseMutex(hMutex);
	return true;
}

/*F+F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F
Function: DeleteTable

Summary:  Delete Table.

Args:     const char* lpDBName
database name
const char* lpTableName
table name

Returns:  BOOL
F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F-F*/
extern "C" bool DeleteTable(const char* lpDBName, const char* lpTableName)
{
	// 从哈希表中查找该数据队列。
	struct TABLE_MSG tabmsg;
	if (!fetchtab(lpDBName, tabmsg))
	{
		return false;
	}

	void* lpMapAddress;
	pthread_mutex_t hMutex;
	lpMapAddress = tabmsg.lpMapAddress;
	hMutex = tabmsg.hMutex;
	DB_HEAD* pHead;
	DB_INDEX_STRUCT* pIndex;
	pHead = (DB_HEAD*)lpMapAddress;
	pIndex = pHead->index;

	//WaitForSingleObject(hMutex, INFINITE);
	std::unique_lock<std::mutex> lock(*tabmsg.pmutex_rw);

	// search table in DB's index(hash table)
	int loc, c;
	loc = hash1(lpTableName);
	c = hash2(lpTableName);
	while (strcmp(pIndex[loc].tablename, "\0") && strcmp(pIndex[loc].tablename, lpTableName))
	{
		loc = (loc + c) % INDEXSIZE;
	}

	if (!strcmp(pIndex[loc].tablename, "\0") || pIndex[loc].erased)
	{
		// table not found
		errorCode = ERROR_TABLE_NOT_EXIST;
		//ReleaseMutex(hMutex);
		return false;
	}

	strcpy(pIndex[loc].tablename, "CT0D9GG_~$59");	 //不是必需的，只是确保安全
	pIndex[loc].erased = true;
	pIndex[loc].currcount = 0;

	int pos = pIndex[loc].startpos;
	int tablesize = pIndex[loc].recordsize * pIndex[loc].maxcount;

	void* Destination = (char*)lpMapAddress + sizeof(DB_HEAD) + pos;
	const void* Source = (char*)Destination + tablesize;
	int Length = pHead->totalsize - sizeof(DB_HEAD) - pos - tablesize;
	memmove(Destination, Source, Length);

	//mark
	int typepos = pIndex[loc].typeaddr;
	int typesize = pIndex[loc].typesize;
	pIndex[loc].typesize = 0;
	Destination = (char*)lpMapAddress + pHead->totalsize + typepos;
	Source = (char*)Destination + typesize;
	Length = INDEXSIZE * TYPEAVGSIZE - typepos - typesize;
	memmove(Destination, Source, Length);

	for (int i = 0; i < INDEXSIZE; i++)
	{
		//if (strcmp(pIndex[i].tablename, "\0") && !pIndex[i].erased && pIndex[i].startpos > pos)
		//{
		//	pIndex[i].startpos -= tablesize;
		//}

		//mark
		if (strcmp(pIndex[i].tablename, "\0") && !pIndex[i].erased)
		{
			if (pIndex[i].startpos > pos)
				pIndex[i].startpos -= tablesize;

			if (pIndex[i].typeaddr > typepos)
				pIndex[i].typeaddr -= typesize;
		}
	}

	pHead->nextpos -= tablesize;
	pHead->remain += tablesize;
	pHead->indexcount--;

	//mark
	pHead->nexttypepos -= typesize;
	pHead->typeremain += typesize;

	//ReleaseMutex(hMutex);
	return true;
}

extern "C" bool ClearDB(const char* lpDbName)
{
	// 从哈希表中查找该数据队列。
	struct TABLE_MSG tabmsg;
	if (!fetchtab(lpDbName, tabmsg))
	{
		return false;
	}

	void* lpMapAddress;
	lpMapAddress = tabmsg.lpMapAddress;
	DB_HEAD* pHead;
	pHead = (DB_HEAD*)lpMapAddress;

	if (pHead->qbdtype != DATABASE_T) return false;

	std::unique_lock<std::mutex> lock(*tabmsg.pmutex_rw);

	int totalsize = pHead->totalsize;
	int typesize = pHead->typesize;
	int fileSize = pHead->totalsize + pHead->typesize;
	//ZeroMemory((char*)pHead->index, sizeof(pHead->index));
	memset((char*)pHead->index, 0, sizeof(pHead->index));
	pHead->qbdtype = DATABASE_T;
	pHead->totalsize = totalsize;	// DB_HEAD和后面数据区大小的和，不包括最后面的类型区
	pHead->typesize = typesize;		// 最后面的类型区的大小 mark
	pHead->remain = totalsize - sizeof(DB_HEAD);
	pHead->typeremain = typesize;
	pHead->counter = 0;
	pHead->nextpos = 0;
	pHead->nexttypepos = 0;
	pHead->indexcount = 0;

	return true;
}

/*F+F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F
Function: CreateItem

Summary:  Create item in Board.

Args:     LPCTSTR lpDBName
database name
LPCTSTR lpItemName
item name to be writen
VOID  *lpItem
item buffer pointer
int actSize
item size
int offset
item offset

Returns:  BOOL
F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F-F*/
extern "C" bool CreateItem(const char* lpBoardName, const char* lpItemName, int itemSize, void* pType, int typeSize)
{
	//目前看是不可能，因为只传入一个C++类型名
	if (typeSize > TYPEMAXSIZE)
	{
		errorCode = ERROR_PARAMETER_SIZE;
		return false;
	}

	// Search bulletin in hash table.
	struct TABLE_MSG tabmsg;
	if (!fetchtab(lpBoardName, tabmsg))
	{
		return false;
	}
	void* lpMapAddress;
	pthread_mutex_t hMutex;
	lpMapAddress = tabmsg.lpMapAddress;
	hMutex = tabmsg.hMutex;
	BOARD_HEAD* pHead;
	BOARD_INDEX_STRUCT* pIndex;
	pHead = (BOARD_HEAD*)lpMapAddress;
	pIndex = pHead->index;

	//WaitForSingleObject(hMutex, INFINITE);
	std::unique_lock<std::mutex> lock(*tabmsg.pmutex_rw);

	//search table in board index(hash table)
	int loc, c, loc_s, c_s;
	loc = hash1(lpItemName);
	c = hash2(lpItemName);
	loc_s = loc, c_s = c;
	while (strcmp(pIndex[loc].itemname, "\0") && strcmp(pIndex[loc].itemname, lpItemName))
	{
		loc = (loc + c) % INDEXSIZE;
	}

	if (strcmp(pIndex[loc].itemname, lpItemName) == 0 && pIndex[loc].erased == false)
	{
		errorCode = ERROR_ITEM_ALREADY_EXIST;
		//ReleaseMutex(hMutex);
		return false;
	}

	// no enough remained space
	if (pHead->remain < itemSize)
	{
		errorCode = ERROR_NO_SPACE;
		//ReleaseMutex(hMutex);
		return false;
	}

	loc = loc_s;
	c = c_s;
	while (strcmp(pIndex[loc].itemname, "\0") && !pIndex[loc].erased)
	{
		loc = (loc + c) % INDEXSIZE;
	}

	if (pHead->indexcount == INDEXSIZE - 1)
	{
		errorCode = ERROR_ITEM_OVERFLOW;
		//ReleaseMutex(hMutex);
		return false;
	}

	pHead->indexcount++;
	strcpy(pIndex[loc].itemname, lpItemName);
	pIndex[loc].itemsize = itemSize;
	pIndex[loc].startpos = pHead->nextpos;
	pIndex[loc].erased = false;
	pHead->nextpos += itemSize;
	pHead->remain -= itemSize;
	//_time64(&pIndex[loc].timestamp);
	clock_gettime(CLOCK_REALTIME, &pIndex[loc].timestamp);
	//mark
	if (pType != 0 && typeSize != 0 && typeSize < TYPEMAXSIZE && typeSize < pHead->typeremain)
	{
		//mark
		memcpy((char*)lpMapAddress + pHead->totalsize + pHead->nexttypepos,
			pType,
			typeSize
		);
		pIndex[loc].typesize = typeSize;
		pIndex[loc].typeaddr = pHead->nexttypepos;
		pHead->nexttypepos += typeSize;
		pHead->typeremain -= typeSize;
	}
	else
	{
		pIndex[loc].typesize = 0;
	}
	memset((char*)lpMapAddress + sizeof(BOARD_HEAD) + pIndex[loc].startpos, 0, itemSize);
	//ReleaseMutex(hMutex);
	return true;
}

/*F+F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F+++F
Function: ReadQueueType

Summary:  Read record type info.

Args:     LPCTSTR  lpDqName
[out] VOID  *lpBuff
pointer of record type buffer
int buffSize
size of buff
[out] int *pTypeSize
type size

Returns:  BOOL
F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F---F-F*/
extern "C" bool ReadType(const char* lpDqName, const char* lpItemName, void* inBuff, int buffSize, int* pTypeSize)
{
	// 从哈希表中查找该数据队列。
	struct TABLE_MSG tabmsg;
	if (!fetchtab(lpDqName, tabmsg))
	{
		*pTypeSize = 0;
		return false;
	}
	void* lpMapAddress;
	lpMapAddress = tabmsg.lpMapAddress;
	int  qbdtype;
	qbdtype = *(int*)lpMapAddress;

	int loc, c;
	if (qbdtype == QUEUE_T)
	{
		QUEUE_HEAD* pHead;
		pHead = (QUEUE_HEAD*)lpMapAddress;

		memcpy(inBuff, (char*)lpMapAddress + pHead->num * (pHead->size + RECORDHEADSIZE) + QUEUEHEADSIZE, pHead->typesize);
		*pTypeSize = pHead->typesize;
	}
	else if (qbdtype == BOARD_T)
	{
		BOARD_HEAD* pHead;
		BOARD_INDEX_STRUCT* pIndex;
		pHead = (BOARD_HEAD*)lpMapAddress;
		pIndex = pHead->index;

		loc = hash1(lpItemName);
		c = hash2(lpItemName);
		// search item in bulletin's index(hash table)
		while (strcmp(pIndex[loc].itemname, "\0") && strcmp(pIndex[loc].itemname, lpItemName))
		{
			loc = (loc + c) % INDEXSIZE;
		}

		if (!strcmp(pIndex[loc].itemname, "\0") || pIndex[loc].erased || pIndex[loc].typesize == 0)
		{
			// item not found
			*pTypeSize = 0;
			return false;
		}

		memcpy(inBuff, (char*)lpMapAddress + pHead->totalsize + pIndex[loc].typeaddr, pIndex[loc].typesize);
		*pTypeSize = pIndex[loc].typesize;
	}
	else if (qbdtype == DATABASE_T)
	{
		DB_HEAD* pHead;
		DB_INDEX_STRUCT* pIndex;
		pHead = (DB_HEAD*)lpMapAddress;
		pIndex = pHead->index;

		loc = hash1(lpItemName);
		c = hash2(lpItemName);
		// search item in bulletin's index(hash table)
		while (strcmp(pIndex[loc].tablename, "\0") && strcmp(pIndex[loc].tablename, lpItemName))
		{
			loc = (loc + c) % INDEXSIZE;
		}

		if (!strcmp(pIndex[loc].tablename, "\0") || pIndex[loc].erased || pIndex[loc].typesize == 0)
		{
			// item not found
			*pTypeSize = 0;
			return false;
		}

		memcpy(inBuff, (char*)lpMapAddress + pHead->totalsize + pIndex[loc].typeaddr, pIndex[loc].typesize);
		*pTypeSize = pIndex[loc].typesize;
	}
	return true;
}

extern "C" bool ReadBoardInfo(const char* lpBoardName, BOARD_INFO* boardinfo)
{
	// 从哈希表中查找该数据队列。
	struct TABLE_MSG tabmsg;
	if (!fetchtab(lpBoardName, tabmsg))
	{
		return false;
	}
	void* lpMapAddress;
	lpMapAddress = tabmsg.lpMapAddress;
	BOARD_HEAD* pHead;
	pHead = (BOARD_HEAD*)lpMapAddress;
	boardinfo->tagcount_head = pHead->indexcount;
	boardinfo->totalsize = pHead->totalsize - sizeof(BOARD_HEAD);
	boardinfo->remainsize = pHead->remain;

	int j = 0;
	for (int i = 0; i < INDEXSIZE; i++)
	{
		if (strcmp(pHead->index[i].itemname, "\0") != 0 && pHead->index[i].erased == false)
		{
			j++;
		}
	}

	boardinfo->tagcount_act = j;

	return true;
}

extern "C" bool readtype(int sockfd, char* qbdname, char* tagname, void* inbuff, int buffsize, int* ptypesize, unsigned int* error)
{
	// 参数校验
	if ((!tagname && !qbdname) || !inbuff || !error || buffsize <= 0) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}

	// 初始化消息头
	MSGSTRUCT msg{};
	msg.head.id = READTYPE;
	msg.head.datasize = buffsize;
	msg.head.bodysize = 0;

	// 安全拷贝字符串
	if (qbdname != 0)
	{
		strncpy(msg.head.qname, qbdname, sizeof(msg.head.qname) - 1);
		msg.head.qname[sizeof(msg.head.qname) - 1] = '\0';
	}

	if (tagname != 0)
	{
		strncpy(msg.head.itemname, tagname, sizeof(msg.head.itemname) - 1);
		msg.head.itemname[sizeof(msg.head.itemname) - 1] = '\0';
	}

	if ( tagname != 0 && tagname[0] != 0)
	{
		//只有一个board
		strncpy(msg.head.qname, "BOARD", sizeof(msg.head.qname) - 1);
		msg.head.qname[sizeof(msg.head.qname) - 1] = '\0';
	}

	// 发送请求
	if (send_all(sockfd, &msg, sizeof(MSGHEAD)) <= 0) {
		*error = errno;
		close(sockfd);
		return false;
	}

	// 读取响应头
	if (readn(sockfd, &msg, sizeof(MSGHEAD)) < 0) {
		*error = errno;
		close(sockfd);
		return false;
	}

	*ptypesize = msg.head.bodysize;

	// 检查错误码
	*error = msg.head.error;
	if (*error != 0 || msg.head.bodysize == 0) {
		return false;
	}

	// 验证数据大小
	if (msg.head.bodysize > buffsize) {
		*error = ERROR_BUFFER_TOO_SMALL;
		close(sockfd);
		return false;
	}

	// 读取数据体（如果有）
	if (msg.head.bodysize > 0) {
		if (readn(sockfd, inbuff, msg.head.bodysize) < 0) {
			*error = errno;
			close(sockfd);
			return false;
		}
	}

	return true;
}

bool writeb_plc(int sockfd, const char* tagname, void* value, int actsize, unsigned int* error)
{
	// 参数校验
	if (!tagname || !value || !error || actsize <= 0) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}

	if (actsize > MAXMSGLEN) {
		*error = ERROR_PARAMETER_SIZE;
		return false;
	}

	// 初始化消息头
	MSGSTRUCT msg{};
	msg.head.id = WRITEBPLC;
	msg.head.datasize = actsize;
	msg.head.bodysize = actsize;
	msg.head.offset = 0;
	msg.head.subsize = 0;

	// 安全拷贝字符串
	strncpy(msg.head.qname, "BOARD", sizeof(msg.head.qname) - 1);
	msg.head.qname[sizeof(msg.head.qname) - 1] = '\0';

	strncpy(msg.head.itemname, tagname, sizeof(msg.head.itemname) - 1);
	msg.head.itemname[sizeof(msg.head.itemname) - 1] = '\0';

	//禁用Nagle算法（适合低延迟场景）后，则执行效率相同
	//memcpy(msg.body, value, actsize);
	//send_all(sockfd, &msg, sizeof(MSGHEAD) + actsize);
	// 发送消息头和数据（单次发送优化）
	if (send_all(sockfd, &msg, sizeof(MSGHEAD)) <= 0 ||
		send_all(sockfd, value, actsize) <= 0) {
		*error = errno;
		close(sockfd);
		return false;
	}

	// 接收响应
	if (readn(sockfd, &msg, sizeof(MSGHEAD)) < 0) {
		*error = errno;
		close(sockfd);
		return false;
	}

	// 验证响应
	if (msg.head.bodysize > 0) {
		*error = ERROR_INVALID_RESPONSE;
		close(sockfd);
		return false;
	}

	*error = msg.head.error;
	return (*error == 0);
}

bool writeb_string_plc(int sockfd, const char* tagname, const char* value, unsigned int* error)
{
	int strlength = strlen((const char*)value);

	// 参数校验
	if (!tagname || !value || !error) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}

	if (strlength > MAXMSGLEN) {
		*error = ERROR_PARAMETER_SIZE;
		return false;
	}
	
	// 初始化消息头
	MSGSTRUCT msg{};
	msg.head.id = WRITEBSTRINGPLC;
	msg.head.datasize = strlength;
	msg.head.bodysize = strlength;
	msg.head.offset = 0;
	msg.head.subsize = 0;

	// 安全拷贝字符串
	strncpy(msg.head.qname, "BOARD", sizeof(msg.head.qname) - 1);
	msg.head.qname[sizeof(msg.head.qname) - 1] = '\0';

	strncpy(msg.head.itemname, tagname, sizeof(msg.head.itemname) - 1);
	msg.head.itemname[sizeof(msg.head.itemname) - 1] = '\0';

	if (send_all(sockfd, &msg, sizeof(MSGHEAD)) <= 0 ||
		send_all(sockfd, value, strlength) <= 0) {
		*error = errno;
		close(sockfd);
		return false;
	}

	// 接收响应
	if (readn(sockfd, &msg, sizeof(MSGHEAD)) < 0) {
		*error = errno;
		close(sockfd);
		return false;
	}

	// 验证响应
	if (msg.head.bodysize > 0) {
		*error = ERROR_INVALID_RESPONSE;
		close(sockfd);
		return false;
	}

	*error = msg.head.error;
	return (*error == 0);
}

extern "C" bool registertag(int sockfd, const char* tagname, unsigned int* error)
{
	// 参数校验
	if (!tagname || !error) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}

	// 初始化消息结构体
	MSGSTRUCT msg{};
	msg.head.id = REGISTERPLCSERVER;
	msg.head.bodysize = 0;

	// 安全拷贝字符串（防止缓冲区溢出）
	strncpy(msg.head.itemname, tagname, sizeof(msg.head.itemname) - 1);
	msg.head.itemname[sizeof(msg.head.itemname) - 1] = '\0';

	strncpy(msg.head.qname, tagname, sizeof(msg.head.qname) - 1);
	msg.head.qname[sizeof(msg.head.qname) - 1] = '\0';

	// 发送请求
	if (send_all(sockfd, &msg, sizeof(MSGHEAD)) <= 0) {
		*error = errno;
		close(sockfd);
		return false;
	}

	// 读取响应
	if (readn(sockfd, &msg, sizeof(MSGHEAD)) < 0) {
		*error = errno;
		close(sockfd);
		return false;
	}

	// 验证响应
	if (msg.head.bodysize > 0) {
		*error = ERROR_INVALID_RESPONSE;
		close(sockfd);
		return false;
	}

	*error = msg.head.error;
	return (*error == 0);
}

extern "C" bool write_plc_string(int sockfd, const char* tagname, std::string str, unsigned int* error)
{
	return writeb_string_plc(sockfd, tagname, str.c_str(), error);
}

extern "C" bool write_plc_bool(int sockfd, const char* tagname, bool value, unsigned int* error)
{
	return writeb_plc(sockfd, tagname, (void*)&value, sizeof(value), error);
}

extern "C" bool write_plc_short(int sockfd, const char* tagname, short value, unsigned int* error)
{
	return writeb_plc(sockfd, tagname, (void*)&value, sizeof(value), error);
}

extern "C" bool write_plc_ushort(int sockfd, const char* tagname, unsigned short value, unsigned int* error)
{
	return writeb_plc(sockfd, tagname, (void*)&value, sizeof(value), error);
}

extern "C" bool write_plc_int(int sockfd, const char* tagname, int value, unsigned int* error)
{
	return writeb_plc(sockfd, tagname, (void*)&value, sizeof(value), error);
}

extern "C" bool write_plc_uint(int sockfd, const char* tagname, unsigned int value, unsigned int* error)
{
	return writeb_plc(sockfd, tagname, (void*)&value, sizeof(value), error);
}

extern "C" bool write_plc_float(int sockfd, const char* tagname, float value, unsigned int* error)
{
	return writeb_plc(sockfd, tagname, (void*)&value, sizeof(value), error);
}