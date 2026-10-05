// testapp6: GplatConnection (C++ 封装类) 测试
// 用法: testapp6 [server_ip] [port]，需要已运行的 gplat 服务
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#include "../include/gplat_connection.h"
#include "../include/type_code.h"
#include "../include/user_types.h"

static std::string g_ip = "127.0.0.1";
static int g_port = 8777;
static std::atomic<int> g_failures(0);

static const char* INT_TAG = "TEST6_INT";
static const char* STR_TAG = "TEST6_STR";
static const int   STR_TAG_SIZE = 64;
static const char* REQ_TAG = "TEST6_REQ";
static const char* RSP_TAG = "TEST6_RSP";
static const char* NOREQ_TAG = "TEST6_NOREQ";
static const char* NORSP_TAG = "TEST6_NORSP";
static const char* QUEUE_NAME = "TEST6_QUEUE";

#define CHECK(cond, ...)                                  \
	do {                                                  \
		if (!(cond)) {                                    \
			printf("[FAIL] %s:%d ", __FILE__, __LINE__); \
			printf(__VA_ARGS__);                          \
			printf("\n");                                 \
			g_failures++;                                 \
		}                                                 \
	} while (0)

// 执行 f：抛出 Ex 时返回其错误码，未抛出返回 0，抛出其它 GplatError 返回 UINT_MAX
template<typename Ex, typename F>
static unsigned int thrownCode(F&& f)
{
	try {
		f();
	}
	catch (const Ex& e) {
		return e.code();
	}
	catch (const GplatError&) {
		return UINT_MAX;
	}
	return 0;
}

static_assert(std::is_base_of_v<std::runtime_error, GplatError>, "GplatError must be a std::runtime_error");
static_assert(std::is_base_of_v<GplatError, GplatUsageError>, "GplatUsageError must derive from GplatError");
static_assert(std::is_base_of_v<GplatError, GplatConnectionError>, "GplatConnectionError must derive from GplatError");
// write_plc_* 的 =delete 模板必须拒绝隐式类型转换
template<typename T, typename = void>
struct CanWritePlcInt : std::false_type {};
template<typename T>
struct CanWritePlcInt<T, std::void_t<decltype(std::declval<GplatConnection&>().write_plc_int("t", std::declval<T>()))>> : std::true_type {};
static_assert(CanWritePlcInt<int>::value, "write_plc_int(int) must compile");
static_assert(!CanWritePlcInt<double>::value, "write_plc_int(double) must be deleted");
static_assert(!CanWritePlcInt<long>::value, "write_plc_int(long) must be deleted");
static_assert(!std::is_copy_constructible_v<GplatConnection>, "GplatConnection must not be copyable");
static_assert(std::is_nothrow_move_constructible_v<GplatConnection>, "GplatConnection must be movable");

static std::string typeDescriptor(int typecode, int arraysize, const char* className = nullptr)
{
	std::string d(8, '\0');
	memcpy(&d[0], &typecode, 4);
	memcpy(&d[4], &arraysize, 4);
	if (className)
		d.append(className, strlen(className) + 1);
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

	bool ok = ensureTag(conn, INT_TAG, sizeof(int32_t), typeDescriptor(Int32, 0)) &&
		ensureTag(conn, STR_TAG, STR_TAG_SIZE, typeDescriptor(Char, STR_TAG_SIZE)) &&
		ensureTag(conn, REQ_TAG, sizeof(Request), typeDescriptor(-1, 0, "Request")) &&
		ensureTag(conn, RSP_TAG, sizeof(Response), typeDescriptor(-1, 0, "Response")) &&
		ensureTag(conn, NOREQ_TAG, sizeof(Request), typeDescriptor(-1, 0, "Request")) &&
		ensureTag(conn, NORSP_TAG, sizeof(Response), typeDescriptor(-1, 0, "Response"));

	if (ok) {
		std::string desc = typeDescriptor(-1, 0, "Request");
		unsigned int error = 0;
		if (!createqueue(conn, QUEUE_NAME, sizeof(Request), 10, NORMAL_MODE, &desc[0], (int)desc.size(), &error) &&
			error != ERROR_ALREADY_LOAD) {
			printf("createqueue %s failed, error=%u\n", QUEUE_NAME, error);
			ok = false;
		}
	}
	disconnectgplat(conn);
	return ok;
}

static void testLifecycle()
{
	printf("[1] open/close/is_open/move\n");
	GplatConnection c(g_ip, g_port);
	CHECK(!c.is_open(), "should not connect in constructor");

	int32_t v = 0;
	CHECK(thrownCode<GplatConnectionError>([&] { (void)c.readb(INT_TAG, &v, sizeof(v)); }) == ERROR_SOCKET_NOT_CONNECTED, "readb before open should throw");
	CHECK(thrownCode<GplatConnectionError>([&] { (void)c.subscribe(INT_TAG); }) == ERROR_SOCKET_NOT_CONNECTED, "subscribe before open should throw");

	CHECK(c.open(), "open failed");
	CHECK(c.open(), "second open should return true");
	CHECK(c.is_open(), "is_open after open");

	GplatConnection moved(std::move(c));
	CHECK(!c.is_open() && moved.is_open(), "move construct");
	CHECK(thrownCode<GplatConnectionError>([&] { (void)c.readb(INT_TAG, &v, sizeof(v)); }) == ERROR_SOCKET_NOT_CONNECTED, "moved-from should be closed");
	CHECK(moved.readb(INT_TAG, &v, sizeof(v)) == 0, "moved-to should work");

	GplatConnection other(g_ip, g_port);
	CHECK(other.open(), "open other failed");
	other = std::move(moved);
	CHECK(!moved.is_open() && other.is_open(), "move assign");
	CHECK(other.readb(INT_TAG, &v, sizeof(v)) == 0, "move-assigned should work");

	other.close();
	CHECK(!other.is_open(), "is_open after close");
	other.close();
	CHECK(other.open() && other.is_open(), "reopen after close");

	GplatConnection bad("127.0.0.1", 1);
	CHECK(!bad.open() && !bad.is_open(), "open to closed port should fail");
}

static void testBoard()
{
	printf("[2] board read/write\n");
	GplatConnection c(g_ip, g_port);
	if (!c.open()) { CHECK(false, "open failed"); return; }

	int32_t w = 12345, r = 0;
	timespec ts{};
	CHECK(c.writeb(INT_TAG, &w, sizeof(w)) == 0, "writeb");
	CHECK(c.readb(INT_TAG, &r, sizeof(r), &ts) == 0 && r == w, "readb r=%d", r);
	CHECK(ts.tv_sec != 0, "readb timestamp not set");

	w = 54321;
	CHECK(c.writeb_notpost(INT_TAG, &w, sizeof(w)) == 0, "writeb_notpost");
	CHECK(c.readb(INT_TAG, &r, sizeof(r)) == 0 && r == w, "readb after notpost r=%d", r);

	char buf[STR_TAG_SIZE];
	CHECK(c.writeb_string(STR_TAG, "hello") == 0, "writeb_string(const char*)");
	CHECK(c.readb_string(STR_TAG, buf, sizeof(buf)) == 0 && strcmp(buf, "hello") == 0, "readb_string(char*) got '%s'", buf);

	std::string s;
	ts = {};
	CHECK(c.writeb_string(STR_TAG, std::string("world")) == 0, "writeb_string(std::string)");
	CHECK(c.readb_string(STR_TAG, s, &ts) == 0 && s == "world", "readb_string(std::string&) got '%s'", s.c_str());
	CHECK(ts.tv_sec != 0, "readb_string timestamp not set");

	CHECK(c.writeb_string_notpost(STR_TAG, "np1") == 0, "writeb_string_notpost(const char*)");
	CHECK(c.readb_string(STR_TAG, s) == 0 && s == "np1", "got '%s'", s.c_str());
	CHECK(c.writeb_string_notpost(STR_TAG, std::string("np2")) == 0, "writeb_string_notpost(std::string)");
	CHECK(c.readb_string(STR_TAG, s) == 0 && s == "np2", "got '%s'", s.c_str());

	// 业务结果：只返回错误码，不抛异常，连接保留
	CHECK(c.readb("TEST6_NO_SUCH_TAG", &r, sizeof(r)) == ERROR_TAG_NOT_EXIST, "readb on missing tag should return ERROR_TAG_NOT_EXIST");
	CHECK(c.is_open(), "server-side error must keep connection open");

	// 编程错误：抛 GplatUsageError；客户端参数校验和服务端报的错误都不关闭连接
	CHECK(thrownCode<GplatUsageError>([&] { (void)c.readb(INT_TAG, &r, 0); }) == ERROR_INVALID_PARAMETER, "readb with actsize 0 should throw GplatUsageError");
	int64_t big = 0;
	CHECK(thrownCode<GplatUsageError>([&] { (void)c.writeb(INT_TAG, &big, sizeof(big)); }) == ERROR_RECORDSIZE, "writeb with wrong size should throw GplatUsageError");
	bool caught = false;
	try {
		(void)c.writeb(INT_TAG, &big, sizeof(big));
	}
	catch (const std::runtime_error& e) {
		caught = strstr(e.what(), ("(Code: " + std::to_string(ERROR_RECORDSIZE) + ")").c_str()) != nullptr;
	}
	CHECK(caught, "GplatUsageError should be catchable as std::runtime_error with the code in what()");
	CHECK(c.is_open(), "usage error must keep connection open");
	CHECK(c.readb(INT_TAG, &r, sizeof(r)) == 0, "connection usable after server error");
}

static void testQueue()
{
	printf("[3] queue read/write/clear/peek\n");
	GplatConnection c(g_ip, g_port);
	if (!c.open()) { CHECK(false, "open failed"); return; }

	CHECK(c.clearq(QUEUE_NAME) == 0, "clearq");
	for (int i = 1; i <= 3; i++) {
		Request req{};
		req.id = i;
		CHECK(c.writeq(QUEUE_NAME, &req, sizeof(req)) == 0, "writeq %d", i);
	}

	Request rec{};
	RECORD_HEAD head{};
	CHECK(c.peekq(QUEUE_NAME, PEEK_NEXT, &rec, sizeof(rec), &head) == 0 && rec.id == 1, "peek next id=%d", rec.id);
	CHECK(c.peekq(QUEUE_NAME, PEEK_LATEST, &rec, sizeof(rec)) == 0 && rec.id == 3, "peek latest id=%d", rec.id);

	for (int i = 1; i <= 3; i++) {
		rec = {};
		CHECK(c.readq(QUEUE_NAME, &rec, sizeof(rec)) == 0 && rec.id == i, "readq expect %d got %d", i, rec.id);
	}
	CHECK(c.readq(QUEUE_NAME, &rec, sizeof(rec)) != 0, "readq on empty queue should fail");
	CHECK(c.is_open(), "empty queue must keep connection open");

	Request req{};
	CHECK(c.writeq(QUEUE_NAME, &req, sizeof(req)) == 0, "writeq before clear");
	CHECK(c.clearq(QUEUE_NAME) == 0, "clearq");
	CHECK(c.readq(QUEUE_NAME, &rec, sizeof(rec)) != 0, "readq after clear should fail");
}

static void testPubSub()
{
	printf("[4] subscribe/waitpostdata\n");
	GplatConnection sub(g_ip, g_port), pub(g_ip, g_port);
	if (!sub.open() || !pub.open()) { CHECK(false, "open failed"); return; }

	CHECK(sub.subscribe(INT_TAG) == 0, "subscribe");
	CHECK(sub.subscribe(STR_TAG) == 0, "subscribe");

	int32_t w = 777;
	CHECK(pub.writeb(INT_TAG, &w, sizeof(w)) == 0, "writeb");
	char buf[256];
	std::string tag;
	CHECK(sub.waitpostdata(tag, buf, sizeof(buf), 1000) == 0 && tag == INT_TAG, "waitpostdata tag=%s", tag.c_str());
	CHECK(*(int32_t*)buf == w, "posted value %d", *(int32_t*)buf);

	CHECK(pub.writeb_notpost(INT_TAG, &w, sizeof(w)) == 0, "writeb_notpost");
	CHECK(sub.waitpostdata(tag, buf, sizeof(buf), 300) == ERROR_WAIT_TIMEOUT && tag == "WAIT_TIMEOUT", "expected timeout, tag=%s", tag.c_str());
	CHECK(sub.is_open(), "timeout must keep connection open");

	{
		GplatConnection delay(g_ip, g_port);
		CHECK(delay.open() && delay.subscribedelaypost(INT_TAG, "TEST6_DELAY_EVENT", 100) == 0, "subscribedelaypost");
	}

	// 缓冲区不足：编程错误，抛 GplatUsageError；libhigplat 读掉并丢弃这条事件，连接和订阅保留
	CHECK(pub.writeb_string(STR_TAG, "a string longer than four bytes") == 0, "writeb_string");
	CHECK(thrownCode<GplatUsageError>([&] { (void)sub.waitpostdata(tag, buf, 4, 1000); }) == ERROR_BUFFER_TOO_SMALL, "waitpostdata with small buffer should throw GplatUsageError");
	CHECK(sub.is_open(), "buffer too small must keep the connection open");
	w = 888;
	CHECK(pub.writeb(INT_TAG, &w, sizeof(w)) == 0, "writeb after dropped event");
	CHECK(sub.waitpostdata(tag, buf, sizeof(buf), 1000) == 0 && tag == INT_TAG && *(int32_t*)buf == w, "subscription kept after dropped event, tag=%s", tag.c_str());

	// subscribe 的服务端错误只返回错误码，连接和已有订阅保留
	CHECK(sub.subscribe("TEST6_NO_SUCH_TAG") == ERROR_TAG_NOT_EXIST, "subscribe on missing tag should return ERROR_TAG_NOT_EXIST");
	CHECK(sub.is_open(), "subscribe server error must keep the connection open");
	CHECK(sub.subscribe(INT_TAG) == 0, "connection usable after subscribe error");
}

static void testGetResponse()
{
	printf("[5] getresponse\n");
	std::atomic<bool> running(true), ready(false);

	std::thread responder([&]() {
		try {
			GplatConnection c(g_ip, g_port);
			if (!c.open() || c.subscribe(REQ_TAG) != 0) {
				CHECK(false, "responder setup failed");
				ready = true;
				return;
			}
			ready = true;

			char buf[1024];
			std::string tag;
			while (running) {
				unsigned int err = c.waitpostdata(tag, buf, sizeof(buf), 200);
				if (err == ERROR_WAIT_TIMEOUT)
					continue;
				if (err != 0) {
					CHECK(false, "responder waitpostdata error=%u", err);
					return;
				}
				if (tag != REQ_TAG)
					continue;
				Request req = read_value<Request>(buf);
				Response rsp{};
				rsp.id = req.id;
				rsp.message = "ok";
				for (int i = 0; i < 4; i++)
					rsp.values[i] = req.args[i] * 2;
				CHECK(c.writeb(RSP_TAG, &rsp, sizeof(rsp)) == 0, "responder writeb");
			}
		}
		catch (const GplatError& e) {
			CHECK(false, "responder: %s", e.what());
			ready = true;
		}
	});
	while (!ready)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));

	GplatConnection c(g_ip, g_port);
	if (c.open()) {
		for (int id = 1; id <= 10; id++) {
			Request req{};
			req.id = id;
			for (int i = 0; i < 4; i++)
				req.args[i] = id + i;
			Response rsp{};
			unsigned int err = c.getresponse(REQ_TAG, &req, sizeof(req), RSP_TAG, &rsp, sizeof(rsp));
			CHECK(err == 0 && rsp.id == id && rsp.values[3] == req.args[3] * 2, "getresponse id=%d err=%u rsp.id=%d", id, err, rsp.id);
		}

		Request req{};
		Response rsp{};
		CHECK(c.getresponse(NOREQ_TAG, &req, sizeof(req), NORSP_TAG, &rsp, sizeof(rsp), 300) == ERROR_RESPONSE_TIMEOUT, "expected response timeout");
		CHECK(c.is_open(), "response timeout must keep connection open");
	}
	else {
		CHECK(false, "open failed");
	}

	running = false;
	responder.join();
}

// 连接类错误：用本地假服务端（accept 后立即关闭）模拟断线，不依赖 gplat
static void testConnectionLost()
{
	printf("[6] connection lost\n");
	int listener = socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	socklen_t len = sizeof(addr);
	if (listener < 0 || bind(listener, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(listener, 1) != 0 ||
		getsockname(listener, (sockaddr*)&addr, &len) != 0) {
		CHECK(false, "fake server setup failed");
		if (listener >= 0)
			::close(listener);
		return;
	}

	GplatConnection c("127.0.0.1", ntohs(addr.sin_port));
	CHECK(c.open(), "open to fake server");
	int peer = accept(listener, nullptr, nullptr);
	if (peer >= 0)
		::close(peer);

	int32_t v = 0;
	unsigned int code = thrownCode<GplatConnectionError>([&] { (void)c.readb(INT_TAG, &v, sizeof(v)); });
	CHECK(code != 0 && code != UINT_MAX && GetErrorCategory(code, nullptr) == GPLAT_ERRCAT_CONNECTION, "readb after peer close should throw GplatConnectionError, code=%u", code);
	CHECK(!c.is_open(), "connection error must close the connection");
	CHECK(thrownCode<GplatConnectionError>([&] { (void)c.subscribe(INT_TAG); }) == ERROR_SOCKET_NOT_CONNECTED, "call after connection error should throw GplatConnectionError");
	::close(listener);
}

int main(int argc, char* argv[])
{
	if (argc > 1) g_ip = argv[1];
	if (argc > 2) g_port = atoi(argv[2]);

	if (!prepare()) {
		printf("prepare failed (is gplat running on %s:%d?)\n", g_ip.c_str(), g_port);
		return 1;
	}

	testLifecycle();
	testBoard();
	testQueue();
	testPubSub();
	testGetResponse();
	testConnectionLost();

	if (g_failures == 0) {
		printf("ALL PASSED\n");
		return 0;
	}
	printf("%d FAILURE(S)\n", g_failures.load());
	return 1;
}
