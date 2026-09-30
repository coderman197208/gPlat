#include "monitor.h"

#include <poll.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <exception>
#include <iostream>
#include <map>
#include <thread>

#include "../include/higplat.h"
#include "display.h"
#include "type_handle.h"

namespace
{

constexpr int kPostBufferSize = 16384 + 1;   // 服务端 MAXMSGLEN + '\0'
constexpr int kKeyPollIntervalMs = 200;
constexpr char kCtrlC = 0x03;

struct TagType
{
	TypeDescriptorHeader header;
	std::string className;
};
using TagTypes = std::map<std::string, TagType>;

// 关闭规范模式与回显以支持单键退出；同时关闭 ISIG，避免 Ctrl+C 终止进程后终端停留在该模式
class RawTerminal
{
public:
	RawTerminal()
	{
		m_active = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &m_saved) == 0;
		if (!m_active)
			return;
		termios raw = m_saved;
		raw.c_lflag &= ~(ICANON | ECHO | ISIG);
		raw.c_cc[VMIN] = 1;
		raw.c_cc[VTIME] = 0;
		tcsetattr(STDIN_FILENO, TCSANOW, &raw);
	}
	~RawTerminal()
	{
		if (m_active)
			tcsetattr(STDIN_FILENO, TCSANOW, &m_saved);
	}
	RawTerminal(const RawTerminal&) = delete;
	RawTerminal& operator=(const RawTerminal&) = delete;

private:
	termios m_saved{};
	bool m_active = false;
};

std::string Now()
{
	timespec now{};
	clock_gettime(CLOCK_REALTIME, &now);
	tm local{};
	localtime_r(&now.tv_sec, &local);
	char text[32];
	const size_t length = strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
	snprintf(text + length, sizeof(text) - length, ".%03ld", now.tv_nsec / 1000000);
	return text;
}

void PrintBanner(const std::string& tagName, const std::string& label)
{
	std::cout << "===== " << tagName << "  " << label << " =====" << std::endl;
}

// waitpostdata 出错时已关闭 conn
void ReceivePosts(int conn, const TagTypes& types, const std::atomic<bool>& stopping, std::atomic<bool>& finished)
{
	std::vector<char> buffer(kPostBufferSize);
	char tagName[GPLAT_TAGNAME_SIZE] = {};
	unsigned int err = 0;
	try
	{
		while (waitpostdata(conn, tagName, sizeof(tagName), buffer.data(), (int)buffer.size(), -1, &err))
		{
			const auto it = types.find(tagName);
			if (it == types.end())
				continue;
			PrintBanner(tagName, Now());
			PrintTagData(it->second.header, it->second.className, buffer.data(), (int)buffer.size());
		}
		if (!stopping)
			std::cout << "Monitor connection lost, error code " << err << "." << std::endl;
	}
	catch (const std::exception& e)
	{
		if (!stopping)
			std::cout << "Monitor failed: " << e.what() << std::endl;
	}
	finished = true;
}

// 返回 true 表示用户要求退出，false 表示接收线程已结束
bool WaitForQuit(const std::atomic<bool>& finished)
{
	pollfd pfd{STDIN_FILENO, POLLIN, 0};
	while (!finished)
	{
		const int ready = poll(&pfd, 1, kKeyPollIntervalMs);
		if (ready < 0 && errno != EINTR)
			return true;
		if (ready <= 0)
			continue;
		char key = 0;
		if (read(STDIN_FILENO, &key, 1) <= 0)
			return true;
		if (key == 'q' || key == 'Q' || key == kCtrlC)
			return true;
	}
	return false;
}

} // namespace

void MonitorTags(int conn, const std::string& host, int port, const std::vector<std::string>& tagNames)
{
	TagTypes types;
	std::vector<std::string> order;
	for (const std::string& tagName : tagNames)
	{
		if (types.count(tagName))
			continue;
		TagType type;
		if (!ReadTagType(conn, tagName, type.header, type.className))
			return;
		types.emplace(tagName, type);
		order.push_back(tagName);
	}

	for (const std::string& tagName : order)
	{
		PrintBanner(tagName, "current");
		PrintTag(conn, tagName);
	}

	// 订阅无法撤销，因此使用独立连接，断开后由服务端清理订阅
	const int monitorConn = connectgplat(host.c_str(), port);
	if (monitorConn <= 0)
	{
		std::cout << "Cannot open monitor connection to " << host << "." << std::endl;
		return;
	}
	for (const std::string& tagName : order)
	{
		unsigned int err = 0;
		if (!subscribe(monitorConn, tagName.c_str(), &err))
		{
			// subscribe 失败时已关闭连接
			std::cout << "Subscribe tag '" << tagName << "' failed, error code " << err << "." << std::endl;
			return;
		}
	}

	std::cout << "Monitoring " << order.size() << " tag(s). Press 'q' to stop." << std::endl;
	std::atomic<bool> stopping{false};
	std::atomic<bool> finished{false};
	std::thread receiver(ReceivePosts, monitorConn, std::cref(types), std::cref(stopping), std::ref(finished));
	{
		RawTerminal terminal;
		if (WaitForQuit(finished))
		{
			stopping = true;
			// 唤醒阻塞在 waitpostdata 中的接收线程，由它关闭连接
			shutdown(monitorConn, SHUT_RDWR);
		}
	}
	receiver.join();
	std::cout << "Monitor stopped." << std::endl;
}
