// testapp7: 服务端发送路径测试（工作线程直接发送、部分发送/EPOLLOUT 续发、积压队列、连接关闭与 fd 复用）
// 用法: testapp7 [server_ip] [port]，需要已运行的 gplat 服务，ProcMsgRecvWorkThreadCount >= 2 时才能覆盖同一连接的并发发送
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "../include/higplat.h"
#include "../include/msg.h"
#include "../include/type_code.h"

static std::string g_ip = "127.0.0.1";
static int g_port = 8777;
static std::atomic<int> g_failures(0);

static const int BIG_TAG_COUNT = 8;
static const char* POST_TAG = "TEST7_POST";
static const int POST_TAG_SIZE = 12000;
static const int POST_INDEX = 100;
static const char* INT_TAG = "TEST7_INT";
static const int32_t INT_VALUE = 7777;
static const int CLIENT_RCVBUF = 4096;	// 客户端接收窗口很小，服务端很快写满发送缓冲区
static const int SMALL_MSS = 536;

#define CHECK(cond, ...)                                  \
	do {                                                  \
		if (!(cond)) {                                    \
			printf("[FAIL] %s:%d ", __FILE__, __LINE__); \
			printf(__VA_ARGS__);                          \
			printf("\n");                                 \
			g_failures++;                                 \
		}                                                 \
	} while (0)

static std::string bigTagName(int i)
{
	return "TEST7_BIG_" + std::to_string(i);
}

static int bigTagSize(int i)
{
	return 9000 + i * 900;
}

static int bigTagIndex(const char* name)
{
	for (int i = 0; i < BIG_TAG_COUNT; i++)
		if (bigTagName(i) == name)
			return i;
	return -1;
}

// 每个 tag 的内容互不相同，且与位置相关，响应错位或交错都能被发现
static std::vector<char> pattern(int seed, int size)
{
	std::vector<char> v(size);
	for (int j = 0; j < size; j++)
		v[j] = (char)(seed * 37 + j * 13 + (j >> 7));
	return v;
}

static std::string typeDescriptor(int typecode, int arraysize)
{
	std::string d(8, '\0');
	memcpy(&d[0], &typecode, 4);
	memcpy(&d[4], &arraysize, 4);
	return d;
}

static bool ensureTag(int conn, const char* tagname, int size, std::string desc)
{
	unsigned int error = 0;
	if (createtag(conn, tagname, size, &desc[0], (int)desc.size(), &error) || error == ERROR_ITEM_ALREADY_EXIST)
		return true;
	printf("createtag %s failed, error=%u\n", tagname, error);
	return false;
}

static bool prepare()
{
	int conn = connectgplat(g_ip.c_str(), g_port);
	if (conn < 0)
		return false;

	unsigned int error = 0;
	bool ok = ensureTag(conn, INT_TAG, sizeof(int32_t), typeDescriptor(Int32, 0)) &&
		ensureTag(conn, POST_TAG, POST_TAG_SIZE, typeDescriptor(Char, POST_TAG_SIZE));
	int32_t v = INT_VALUE;
	ok = ok && writeb(conn, INT_TAG, &v, sizeof(v), &error);
	for (int i = 0; ok && i < BIG_TAG_COUNT; i++) {
		std::string name = bigTagName(i);
		std::vector<char> data = pattern(i, bigTagSize(i));
		ok = ensureTag(conn, name.c_str(), bigTagSize(i), typeDescriptor(Char, bigTagSize(i))) &&
			writeb_notpost(conn, name.c_str(), data.data(), (int)data.size(), &error);
	}
	disconnectgplat(conn);
	return ok;
}

// rcvbuf > 0 时在 connect 前设置接收缓冲区；所有读操作 5 秒超时，服务端漏发/卡住会表现为超时
static int rawConnect(int rcvbuf)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	if (rcvbuf > 0) {
		setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
		// 通告小 MSS，服务端按 MSS 自动调整的发送缓冲区随之变小，发送缓冲区会反复写满
		int mss = SMALL_MSS;
		setsockopt(fd, IPPROTO_TCP, TCP_MAXSEG, &mss, sizeof(mss));
	}
	timeval tv{ 5, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)g_port);
	inet_pton(AF_INET, g_ip.c_str(), &addr.sin_addr);
	if (connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
		close(fd);
		return -1;
	}
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	return fd;
}

static bool sendAll(int fd, const void* buf, size_t len)
{
	const char* p = (const char*)buf;
	while (len > 0) {
		ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
		if (n <= 0)
			return false;
		p += n;
		len -= (size_t)n;
	}
	return true;
}

static bool recvAll(int fd, void* buf, size_t len)
{
	char* p = (char*)buf;
	while (len > 0) {
		ssize_t n = recv(fd, p, len, 0);
		if (n <= 0)
			return false;
		p += n;
		len -= (size_t)n;
	}
	return true;
}

static MSGHEAD readBRequest(int idx)
{
	MSGHEAD h{};
	h.id = READB;
	strcpy(h.qname, "BOARD");
	strcpy(h.itemname, bigTagName(idx).c_str());
	h.datasize = bigTagSize(idx);
	return h;
}

// 收一个完整响应并校验，返回大 tag 下标或 POST_INDEX，失败返回 -1
static int readAndCheck(int fd, const char* ctx)
{
	MSGHEAD h;
	if (!recvAll(fd, &h, sizeof(h))) {
		CHECK(false, "%s: recv header failed or timed out (errno=%d)", ctx, errno);
		return -1;
	}
	if (h.bodysize < 0 || h.bodysize > MAXMSGLEN) {
		CHECK(false, "%s: bad bodysize %d, stream corrupted", ctx, h.bodysize);
		return -1;
	}
	std::vector<char> body(h.bodysize);
	if (h.bodysize > 0 && !recvAll(fd, body.data(), h.bodysize)) {
		CHECK(false, "%s: recv body failed or timed out (errno=%d)", ctx, errno);
		return -1;
	}
	h.itemname[sizeof(h.itemname) - 1] = '\0';

	if (h.id == READB) {
		int idx = bigTagIndex(h.itemname);
		if (idx < 0 || h.error != 0 || body != pattern(idx, bigTagSize(idx))) {
			CHECK(false, "%s: bad READB response tag=%s error=%u bodysize=%d", ctx, h.itemname, h.error, h.bodysize);
			return -1;
		}
		return idx;
	}
	if (h.id == POST && strcmp(h.itemname, POST_TAG) == 0) {
		if (body != pattern(POST_INDEX, POST_TAG_SIZE)) {
			CHECK(false, "%s: bad POST body", ctx);
			return -1;
		}
		return POST_INDEX;
	}
	CHECK(false, "%s: unexpected message id=%d tag=%s", ctx, h.id, h.itemname);
	return -1;
}

// 一次性发出 reqs 个 READB，暂停不读让服务端积压，再慢速读回并逐个校验
static void pipelinedClient(int seed, int reqs)
{
	char ctx[64];
	snprintf(ctx, sizeof(ctx), "pipeline#%d", seed);
	int fd = rawConnect(CLIENT_RCVBUF);
	if (fd < 0) {
		CHECK(false, "%s: connect failed", ctx);
		return;
	}

	std::mt19937 rng(seed);
	std::vector<MSGHEAD> batch;
	int expected[BIG_TAG_COUNT] = { 0 };
	for (int i = 0; i < reqs; i++) {
		int idx = (int)(rng() % BIG_TAG_COUNT);
		expected[idx]++;
		batch.push_back(readBRequest(idx));
	}
	CHECK(sendAll(fd, batch.data(), batch.size() * sizeof(MSGHEAD)), "%s: send requests failed", ctx);
	std::this_thread::sleep_for(std::chrono::milliseconds(300));

	int got[BIG_TAG_COUNT] = { 0 };
	for (int i = 0; i < reqs; i++) {
		int idx = readAndCheck(fd, ctx);
		if (idx < 0 || idx >= BIG_TAG_COUNT) {
			CHECK(idx >= 0, "%s: stopped after %d of %d responses", ctx, i, reqs);
			break;
		}
		got[idx]++;
		if (i % 32 == 0)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	for (int i = 0; i < BIG_TAG_COUNT; i++)
		CHECK(got[i] == expected[i], "%s: tag %d expected %d responses, got %d", ctx, i, expected[i], got[i]);
	close(fd);
}

static void testPipelined()
{
	// 单连接时服务端没有其他流量，积压消息若在 EPOLLOUT 完成后没被唤醒发送，会表现为读超时
	printf("[1] pipelined READB, single connection, slow reader\n");
	pipelinedClient(1, 600);

	printf("[2] pipelined READB, 4 connections concurrently\n");
	std::vector<std::thread> ths;
	for (int c = 0; c < 4; c++)
		ths.emplace_back(pipelinedClient, 10 + c, 600);
	for (auto& t : ths)
		t.join();
}

// 反复发一小批请求再全部读回：每批的响应由多个工作线程几乎同时发送，恰好在发送缓冲区写满的边界上，
// 用来暴露同一连接上多个线程并发 send() 的问题（交错、剩余数据被覆盖）
static void burstClient(int seed, int bursts, int perBurst)
{
	char ctx[64];
	snprintf(ctx, sizeof(ctx), "burst#%d", seed);
	int fd = rawConnect(CLIENT_RCVBUF);
	if (fd < 0) {
		CHECK(false, "%s: connect failed", ctx);
		return;
	}
	std::vector<MSGHEAD> batch;
	for (int i = 0; i < perBurst; i++)
		batch.push_back(readBRequest(BIG_TAG_COUNT - 1 - i % 2));

	for (int b = 0; b < bursts; b++) {
		if (!sendAll(fd, batch.data(), batch.size() * sizeof(MSGHEAD))) {
			CHECK(false, "%s: send failed in burst %d", ctx, b);
			break;
		}
		int i = 0;
		for (; i < perBurst; i++)
			if (readAndCheck(fd, ctx) < 0)
				break;
		if (i < perBurst) {
			CHECK(false, "%s: burst %d stopped after %d of %d responses", ctx, b, i, perBurst);
			break;
		}
	}
	close(fd);
}

static void testBursts()
{
	printf("[3] repeated bursts at the send-buffer-full boundary, 4 connections\n");
	std::vector<std::thread> ths;
	for (int c = 0; c < 4; c++)
		ths.emplace_back(burstClient, 20 + c, 200, 8);
	for (auto& t : ths)
		t.join();
}

// 连接上积压大量响应时，另一个连接写入触发 POST，POST 与响应必须完整、不交错
static void testPostWhileBacklogged()
{
	printf("[4] POST from another connection while responses are backlogged\n");
	const int reqs = 400;
	int fd = rawConnect(CLIENT_RCVBUF);
	if (fd < 0) {
		CHECK(false, "connect failed");
		return;
	}
	unsigned int error = 0;
	if (!subscribe(fd, POST_TAG, &error)) {
		CHECK(false, "subscribe failed, error=%u", error);
		return;
	}

	std::vector<MSGHEAD> batch;
	MSGHEAD wait{};
	wait.id = POSTWAIT;
	wait.timeout = -1;
	batch.push_back(wait);
	int expected[BIG_TAG_COUNT] = { 0 };
	for (int i = 0; i < reqs; i++) {
		int idx = i % BIG_TAG_COUNT;
		expected[idx]++;
		batch.push_back(readBRequest(idx));
	}
	CHECK(sendAll(fd, batch.data(), batch.size() * sizeof(MSGHEAD)), "send requests failed");
	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	int writer = connectgplat(g_ip.c_str(), g_port);
	std::vector<char> payload = pattern(POST_INDEX, POST_TAG_SIZE);
	CHECK(writer >= 0 && writeb(writer, POST_TAG, payload.data(), POST_TAG_SIZE, &error), "writeb %s failed, error=%u", POST_TAG, error);
	disconnectgplat(writer);
	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	int got[BIG_TAG_COUNT] = { 0 };
	int posts = 0;
	for (int i = 0; i < reqs + 1; i++) {
		int idx = readAndCheck(fd, "post");
		if (idx < 0) {
			CHECK(false, "post: stopped after %d of %d messages", i, reqs + 1);
			break;
		}
		if (idx == POST_INDEX)
			posts++;
		else
			got[idx]++;
		if (i % 32 == 0)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	CHECK(posts == 1, "expected 1 POST, got %d", posts);
	for (int i = 0; i < BIG_TAG_COUNT; i++)
		CHECK(got[i] == expected[i], "post: tag %d expected %d responses, got %d", i, expected[i], got[i]);
	close(fd);
}

// 服务端还在发送时客户端断开，紧接着的新连接很可能复用同一个 fd 号，不能收到旧连接的数据
static void testCloseWhileSending()
{
	printf("[5] disconnect while sending, then new connection reuses fd\n");
	for (int round = 0; round < 30; round++) {
		int fd = rawConnect(CLIENT_RCVBUF);
		if (fd < 0) {
			CHECK(false, "round %d: connect failed", round);
			return;
		}
		std::vector<MSGHEAD> batch;
		for (int i = 0; i < 200; i++)
			batch.push_back(readBRequest(i % BIG_TAG_COUNT));
		sendAll(fd, batch.data(), batch.size() * sizeof(MSGHEAD));
		std::this_thread::sleep_for(std::chrono::milliseconds(round % 3));
		if (round % 2) {
			linger lg{ 1, 0 };	// 发 RST
			setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
		}
		close(fd);

		int fd2 = rawConnect(0);
		if (fd2 < 0) {
			CHECK(false, "round %d: reconnect failed", round);
			return;
		}
		for (int k = 0; k < 10; k++) {
			unsigned int error = 0;
			int32_t v = 0;
			if (!readb(fd2, INT_TAG, &v, sizeof(v), &error, nullptr) || v != INT_VALUE) {
				CHECK(false, "round %d: readb %s failed error=%u value=%d", round, INT_TAG, error, v);
				return;
			}
			int idx = k % BIG_TAG_COUNT;
			std::vector<char> data(bigTagSize(idx));
			if (!readb(fd2, bigTagName(idx).c_str(), data.data(), (int)data.size(), &error, nullptr) ||
				data != pattern(idx, bigTagSize(idx))) {
				CHECK(false, "round %d: readb %s failed or corrupted, error=%u", round, bigTagName(idx).c_str(), error);
				return;
			}
		}
		close(fd2);
	}
}

int main(int argc, char* argv[])
{
	if (argc > 1) g_ip = argv[1];
	if (argc > 2) g_port = atoi(argv[2]);

	if (!prepare()) {
		printf("prepare failed (is gplat running on %s:%d?)\n", g_ip.c_str(), g_port);
		return 1;
	}

	auto start = std::chrono::steady_clock::now();
	testPipelined();
	testBursts();
	testPostWhileBacklogged();
	testCloseWhileSending();

	int fd = rawConnect(0);
	unsigned int error = 0;
	int32_t v = 0;
	CHECK(fd >= 0 && readb(fd, INT_TAG, &v, sizeof(v), &error, nullptr) && v == INT_VALUE, "server not responsive after tests, error=%u", error);
	if (fd >= 0)
		close(fd);

	long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
	if (g_failures == 0) {
		printf("ALL PASSED (%lld ms)\n", ms);
		return 0;
	}
	printf("%d FAILURE(S)\n", g_failures.load());
	return 1;
}
