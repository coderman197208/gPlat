// testapp5: getresponse 请求-响应测试
// 一个响应线程订阅请求 tag 并写回响应，请求方在其它线程/连接上调用 getresponse
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../include/higplat.h"
#include "../include/user_types.h"

static const char* SERVER_IP = "127.0.0.1";
static const int   SERVER_PORT = 8777;

static const char* REQ_TAG = "TEST5_REQ";
static const char* RSP_TAG = "TEST5_RSP";
static const char* SLOW_REQ_TAG = "TEST5_SLOW_REQ";
static const char* SLOW_RSP_TAG = "TEST5_SLOW_RSP";

static const int SLOW_TIMEOUT_MS = 500;		// 慢请求的 getresponse 超时
static const int SLOW_DELAY_MS = 1500;		// 响应方故意延迟，超过超时时间

static std::atomic<bool> g_running(true);
static std::atomic<bool> g_responderReady(false);
static std::atomic<int>  g_failures(0);

#define CHECK(cond, ...)                         \
	do {                                         \
		if (!(cond)) {                           \
			printf("[FAIL] %s:%d ", __FILE__, __LINE__); \
			printf(__VA_ARGS__);                 \
			printf("\n");                        \
			g_failures++;                        \
		}                                        \
	} while (0)

using Clock = std::chrono::steady_clock;

static long long elapsedMs(Clock::time_point start)
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

static bool ensureTag(int conn, const char* tagname, const char* typeName, int size)
{
	// 类型元数据：[typecode=-1][arraysize=0][classname\0]
	char buff[128] = { 0 };
	*(int*)buff = -1;
	*(int*)(buff + 4) = 0;
	strcpy(buff + 8, typeName);
	int typesize = 8 + (int)strlen(typeName) + 1;

	unsigned int error = 0;
	if (createtag(conn, tagname, size, buff, typesize, &error) || error == ERROR_ITEM_ALREADY_EXIST)
	{
		return true;
	}
	printf("createtag %s failed, error=%u\n", tagname, error);
	return false;
}

static Request makeRequest(int id)
{
	Request req{};
	req.id = id;
	req.cmd = 1;
	req.name = "req_" + std::to_string(id);
	for (int i = 0; i < 4; i++)
	{
		req.args[i] = id + i * 0.5;
	}
	return req;
}

static bool verifyResponse(const Request& req, const Response& rsp)
{
	if (rsp.id != req.id || rsp.result != 0)
	{
		return false;
	}
	for (int i = 0; i < 4; i++)
	{
		if (rsp.values[i] != req.args[i] * 2)
		{
			return false;
		}
	}
	return true;
}

// 发一次请求并校验一一对应
static bool requestOnce(int conn, int id)
{
	Request req = makeRequest(id);
	Response rsp{};
	unsigned int error = 0;
	bool ok = getresponse(conn, REQ_TAG, &req, sizeof(req), RSP_TAG, &rsp, sizeof(rsp), &error);
	CHECK(ok, "getresponse id=%d failed, error=%u", id, error);
	if (!ok)
	{
		return false;
	}
	CHECK(verifyResponse(req, rsp), "response mismatch: req.id=%d rsp.id=%d", req.id, rsp.id);
	return true;
}

static void responderThread()
{
	int conn = connectgplat(SERVER_IP, SERVER_PORT);
	if (conn < 0)
	{
		printf("responder connect failed\n");
		g_failures++;
		g_responderReady = true;
		return;
	}

	unsigned int error = 0;
	subscribe(conn, REQ_TAG, &error);
	subscribe(conn, SLOW_REQ_TAG, &error);
	// response_tag 的写入不应通知普通订阅者，订阅它用于验证
	subscribe(conn, RSP_TAG, &error);
	g_responderReady = true;

	char buf[4096];
	std::string tag;
	while (g_running)
	{
		if (!waitpostdata(conn, tag, buf, sizeof(buf), 200, &error))
		{
			printf("responder waitpostdata failed, error=%u\n", error);
			g_failures++;
			break;
		}
		if (tag == "WAIT_TIMEOUT")
		{
			continue;
		}

		CHECK(tag != RSP_TAG, "response tag %s should not be posted to normal subscribers", RSP_TAG);
		if (tag != REQ_TAG && tag != SLOW_REQ_TAG)
		{
			continue;
		}

		Request req = read_value<Request>(buf);
		Response rsp{};
		rsp.id = req.id;
		rsp.result = 0;
		rsp.message = "ok";
		for (int i = 0; i < 4; i++)
		{
			rsp.values[i] = req.args[i] * 2;
		}

		if (tag == REQ_TAG)
		{
			// 处理期间请求 tag 不应被其它同名请求覆盖（验证串行化）
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
			Request current{};
			readb(conn, REQ_TAG, &current, sizeof(current), &error);
			CHECK(current.id == req.id, "request overwritten while processing: got %d, now %d", req.id, current.id);

			writeb(conn, RSP_TAG, &rsp, sizeof(rsp), &error);
		}
		else
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(SLOW_DELAY_MS));
			writeb(conn, SLOW_RSP_TAG, &rsp, sizeof(rsp), &error);
			printf("  responder: late response for id=%d sent (server should log a discard)\n", req.id);
		}
	}

	disconnectgplat(conn);
}

static void testBasic()
{
	printf("[1] basic request/response\n");
	int conn = connectgplat(SERVER_IP, SERVER_PORT);
	CHECK(conn >= 0, "connect failed");
	if (conn < 0) return;

	for (int id = 1; id <= 20; id++)
	{
		if (!requestOnce(conn, id)) break;
	}
	disconnectgplat(conn);
}

static void testPendingEvents()
{
	printf("[2] getresponse with other subscriptions pending\n");
	int conn = connectgplat(SERVER_IP, SERVER_PORT);
	CHECK(conn >= 0, "connect failed");
	if (conn < 0) return;

	unsigned int error = 0;
	CHECK(subscribe(conn, "timer_500ms", &error), "subscribe timer_500ms failed, error=%u", error);
	// 让定时事件在服务端排队
	std::this_thread::sleep_for(std::chrono::milliseconds(1200));

	for (int id = 101; id <= 110; id++)
	{
		if (!requestOnce(conn, id)) break;
	}

	char buf[256];
	std::string tag;
	bool ok = waitpostdata(conn, tag, buf, sizeof(buf), 1000, &error);
	CHECK(ok && tag == "timer_500ms", "queued event not received after getresponse, tag=%s error=%u", tag.c_str(), error);
	disconnectgplat(conn);
}

static void testConcurrent()
{
	printf("[3] concurrent requests on the same request tag\n");
	const int threadCount = 4;
	const int perThread = 25;

	std::vector<std::thread> threads;
	for (int t = 0; t < threadCount; t++)
	{
		threads.emplace_back([t, perThread]() {
			int conn = connectgplat(SERVER_IP, SERVER_PORT);
			CHECK(conn >= 0, "connect failed");
			if (conn < 0) return;
			for (int i = 0; i < perThread; i++)
			{
				if (!requestOnce(conn, (t + 1) * 10000 + i)) break;
			}
			disconnectgplat(conn);
		});
	}
	for (auto& th : threads)
	{
		th.join();
	}
}

static void testTimeout()
{
	printf("[4] timeout and late response\n");
	int conn = connectgplat(SERVER_IP, SERVER_PORT);
	CHECK(conn >= 0, "connect failed");
	if (conn < 0) return;

	Request req = makeRequest(900);
	Response rsp{};
	unsigned int error = 0;
	auto start = Clock::now();
	bool ok = getresponse(conn, SLOW_REQ_TAG, &req, sizeof(req), SLOW_RSP_TAG, &rsp, sizeof(rsp), &error, SLOW_TIMEOUT_MS);
	long long cost = elapsedMs(start);

	CHECK(!ok && error == ERROR_RESPONSE_TIMEOUT, "expected timeout, ok=%d error=%u", ok, error);
	CHECK(cost >= SLOW_TIMEOUT_MS - 50 && cost < SLOW_TIMEOUT_MS + 500, "timeout took %lld ms", cost);

	// 等迟到的响应写入，服务端应记录丢弃日志
	std::this_thread::sleep_for(std::chrono::milliseconds(SLOW_DELAY_MS));

	// 超时后连接仍可正常使用
	requestOnce(conn, 901);
	disconnectgplat(conn);
}

int main()
{
	int conn = connectgplat(SERVER_IP, SERVER_PORT);
	if (conn < 0)
	{
		printf("connect gplat failed\n");
		return 1;
	}
	bool tagsOk = ensureTag(conn, REQ_TAG, "Request", sizeof(Request)) &&
		ensureTag(conn, RSP_TAG, "Response", sizeof(Response)) &&
		ensureTag(conn, SLOW_REQ_TAG, "Request", sizeof(Request)) &&
		ensureTag(conn, SLOW_RSP_TAG, "Response", sizeof(Response));
	disconnectgplat(conn);
	if (!tagsOk)
	{
		return 1;
	}

	std::thread responder(responderThread);
	while (!g_responderReady)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	testBasic();
	testPendingEvents();
	testConcurrent();
	testTimeout();

	g_running = false;
	responder.join();

	if (g_failures == 0)
	{
		printf("ALL PASSED\n");
		return 0;
	}
	printf("FAILED: %d\n", g_failures.load());
	return 1;
}
