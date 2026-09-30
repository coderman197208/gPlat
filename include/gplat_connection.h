#if !defined(GPLAT_CONNECTION_H_INCLUDED_)
#define GPLAT_CONNECTION_H_INCLUDED_

#include <ctime>
#include <string>
#include <utility>

#include "higplat.h"

// C++ wrapper of the gPlat network API.
// Header-only on purpose: libhigplat.so keeps exporting only the extern "C" API, so this class adds no ABI surface.
// Methods return the error code (0 = success). Not thread-safe: use one connection per thread.
class GplatConnection
{
public:
	GplatConnection(std::string server, int port) : m_server(std::move(server)), m_port(port) {}
	~GplatConnection() { close(); }

	GplatConnection(const GplatConnection&) = delete;
	GplatConnection& operator=(const GplatConnection&) = delete;

	GplatConnection(GplatConnection&& other) noexcept
		: m_server(std::move(other.m_server)), m_port(other.m_port), m_sockfd(std::exchange(other.m_sockfd, -1)) {}

	GplatConnection& operator=(GplatConnection&& other) noexcept
	{
		if (this != &other) {
			close();
			m_server = std::move(other.m_server);
			m_port = other.m_port;
			m_sockfd = std::exchange(other.m_sockfd, -1);
		}
		return *this;
	}

	// Reflects local state only; a peer-side close is detected by the next failing call.
	bool is_open() const { return m_sockfd >= 0; }

	bool open()
	{
		if (!is_open())
			m_sockfd = ::connectgplat(m_server.c_str(), m_port);
		return is_open();
	}

	void close()
	{
		if (m_sockfd >= 0) {
			::disconnectgplat(m_sockfd);
			m_sockfd = -1;
		}
	}

	// ---- Queue ----
	[[nodiscard]] unsigned int readq(const std::string& qname, void* record, int actsize)
	{
		return call(CloseRule::Default, [&](int fd, unsigned int* err) {
			return ::readq(fd, qname.c_str(), record, actsize, err);
		});
	}

	[[nodiscard]] unsigned int writeq(const std::string& qname, const void* record, int actsize)
	{
		return call(CloseRule::Default, [&](int fd, unsigned int* err) {
			return ::writeq(fd, qname.c_str(), const_cast<void*>(record), actsize, err);
		});
	}

	[[nodiscard]] unsigned int clearq(const std::string& qname)
	{
		return call(CloseRule::Default, [&](int fd, unsigned int* err) {
			return ::clearq(fd, qname.c_str(), err);
		});
	}

	[[nodiscard]] unsigned int peekq(const std::string& qname, int position, void* record, int actsize, RECORD_HEAD* recordhead = nullptr)
	{
		return call(CloseRule::Default, [&](int fd, unsigned int* err) {
			return ::peekq(fd, qname.c_str(), position, record, actsize, recordhead, err);
		});
	}

	// ---- Board ----
	[[nodiscard]] unsigned int readb(const std::string& tagname, void* value, int actsize, timespec* timestamp = nullptr)
	{
		return call(CloseRule::Default, [&](int fd, unsigned int* err) {
			return ::readb(fd, tagname.c_str(), value, actsize, err, timestamp);
		});
	}

	[[nodiscard]] unsigned int writeb(const std::string& tagname, const void* value, int actsize)
	{
		return call(CloseRule::Default, [&](int fd, unsigned int* err) {
			return ::writeb(fd, tagname.c_str(), const_cast<void*>(value), actsize, err);
		});
	}

	[[nodiscard]] unsigned int writeb_notpost(const std::string& tagname, const void* value, int actsize)
	{
		return call(CloseRule::Default, [&](int fd, unsigned int* err) {
			return ::writeb_notpost(fd, tagname.c_str(), const_cast<void*>(value), actsize, err);
		});
	}

	[[nodiscard]] unsigned int readb_string(const std::string& tagname, char* value, int buffersize, timespec* timestamp = nullptr)
	{
		return call(CloseRule::Default, [&](int fd, unsigned int* err) {
			return ::readb_string(fd, tagname.c_str(), value, buffersize, err, timestamp);
		});
	}

	[[nodiscard]] unsigned int readb_string(const std::string& tagname, std::string& value, timespec* timestamp = nullptr)
	{
		return call(CloseRule::Default, [&](int fd, unsigned int* err) {
			return ::readb_string2(fd, tagname.c_str(), value, err, timestamp);
		});
	}

	[[nodiscard]] unsigned int writeb_string(const std::string& tagname, const char* value)
	{
		return call(CloseRule::Default, [&](int fd, unsigned int* err) {
			return ::writeb_string(fd, tagname.c_str(), value, err);
		});
	}

	[[nodiscard]] unsigned int writeb_string(const std::string& tagname, const std::string& value)
	{
		return call(CloseRule::Default, [&](int fd, unsigned int* err) {
			return ::writeb_string2(fd, tagname.c_str(), value, err);
		});
	}

	[[nodiscard]] unsigned int writeb_string_notpost(const std::string& tagname, const char* value)
	{
		return call(CloseRule::Default, [&](int fd, unsigned int* err) {
			return ::writeb_string_notpost(fd, tagname.c_str(), value, err);
		});
	}

	[[nodiscard]] unsigned int writeb_string_notpost(const std::string& tagname, const std::string& value)
	{
		return writeb_string_notpost(tagname, value.c_str());
	}

	// ---- Pub/Sub ----
	[[nodiscard]] unsigned int subscribe(const std::string& tagname)
	{
		return call(CloseRule::AnyServerError, [&](int fd, unsigned int* err) {
			return ::subscribe(fd, tagname.c_str(), err);
		});
	}

	[[nodiscard]] unsigned int subscribedelaypost(const std::string& tagname, const std::string& eventname, int delaytime)
	{
		return call(CloseRule::Default, [&](int fd, unsigned int* err) {
			return ::subscribedelaypost(fd, tagname.c_str(), eventname.c_str(), delaytime, err);
		});
	}

	// On timeout returns ERROR_WAIT_TIMEOUT and sets tagname to "WAIT_TIMEOUT".
	[[nodiscard]] unsigned int waitpostdata(std::string& tagname, void* value, int buffersize, int timeout)
	{
		return call(CloseRule::AnyServerError, [&](int fd, unsigned int* err) {
			return ::waitpostdata(fd, tagname, value, buffersize, timeout, err);
		});
	}

	// ---- Request/Response ----
	[[nodiscard]] unsigned int getresponse(const std::string& request_tag, const void* request_value, int request_size,
		const std::string& response_tag, void* response_value, int response_size, int timeout_ms = 2000)
	{
		return call(CloseRule::Default, [&](int fd, unsigned int* err) {
			return ::getresponse(fd, request_tag.c_str(), const_cast<void*>(request_value), request_size,
				response_tag.c_str(), response_value, response_size, err, timeout_ms);
		});
	}

	// ---- PLC ----
	[[nodiscard]] unsigned int write_plc_string(const std::string& tagname, const std::string& str)
	{
		return call(CloseRule::Default, [&](int fd, unsigned int* err) {
			return ::write_plc_string(fd, tagname.c_str(), str, err);
		});
	}

	[[nodiscard]] unsigned int write_plc_bool(const std::string& tagname, bool value)
	{
		return call(CloseRule::Default, [&](int fd, unsigned int* err) {
			return ::write_plc_bool(fd, tagname.c_str(), value, err);
		});
	}
	template<typename T> unsigned int write_plc_bool(const std::string& tagname, T value) = delete;

	[[nodiscard]] unsigned int write_plc_short(const std::string& tagname, short value)
	{
		return call(CloseRule::Default, [&](int fd, unsigned int* err) {
			return ::write_plc_short(fd, tagname.c_str(), value, err);
		});
	}
	template<typename T> unsigned int write_plc_short(const std::string& tagname, T value) = delete;

	[[nodiscard]] unsigned int write_plc_ushort(const std::string& tagname, unsigned short value)
	{
		return call(CloseRule::Default, [&](int fd, unsigned int* err) {
			return ::write_plc_ushort(fd, tagname.c_str(), value, err);
		});
	}
	template<typename T> unsigned int write_plc_ushort(const std::string& tagname, T value) = delete;

	[[nodiscard]] unsigned int write_plc_int(const std::string& tagname, int value)
	{
		return call(CloseRule::Default, [&](int fd, unsigned int* err) {
			return ::write_plc_int(fd, tagname.c_str(), value, err);
		});
	}
	template<typename T> unsigned int write_plc_int(const std::string& tagname, T value) = delete;

	[[nodiscard]] unsigned int write_plc_uint(const std::string& tagname, unsigned int value)
	{
		return call(CloseRule::Default, [&](int fd, unsigned int* err) {
			return ::write_plc_uint(fd, tagname.c_str(), value, err);
		});
	}
	template<typename T> unsigned int write_plc_uint(const std::string& tagname, T value) = delete;

	[[nodiscard]] unsigned int write_plc_float(const std::string& tagname, float value)
	{
		return call(CloseRule::Default, [&](int fd, unsigned int* err) {
			return ::write_plc_float(fd, tagname.c_str(), value, err);
		});
	}
	template<typename T> unsigned int write_plc_float(const std::string& tagname, T value) = delete;

private:
	// How to tell that the C function already closed the socket on failure.
	enum class CloseRule
	{
		Default,		// only on I/O errors and client-detected protocol errors
		AnyServerError	// subscribe / waitpostdata also close on server-reported errors
	};

	static bool closed_by_library(CloseRule rule, unsigned int error)
	{
		if (error == ERROR_INVALID_PARAMETER || error == ERROR_WAIT_TIMEOUT)
			return false;
		if (rule == CloseRule::AnyServerError)
			return true;
		// error < MY_ERR_OFFSET is an errno; 0 on failure means send() returned 0 without errno
		return error < MY_ERR_OFFSET ||
			error == ERROR_SOCKET_NOT_CONNECTED ||
			error == ERROR_INVALID_RESPONSE ||
			error == ERROR_BUFFER_TOO_SMALL;
	}

	template<typename F>
	unsigned int call(CloseRule rule, F&& f)
	{
		if (!is_open())
			return ERROR_SOCKET_NOT_CONNECTED;

		unsigned int error = 0;
		bool ok = false;
		try {
			ok = f(m_sockfd, &error);
		}
		catch (...) {
			// libhigplat throws on fatal error codes after it may have closed the socket
			if (error != 0 && closed_by_library(rule, error))
				m_sockfd = -1;
			throw;
		}

		if (ok)
			return error;	// non-zero only for waitpostdata timeout
		if (closed_by_library(rule, error))
			m_sockfd = -1;
		return error != 0 ? error : ERROR_SOCKET_NOT_CONNECTED;
	}

	std::string m_server;
	int m_port = 0;
	int m_sockfd = -1;
};

#endif // GPLAT_CONNECTION_H_INCLUDED_
