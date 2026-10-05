#if !defined(GPLAT_CONNECTION_H_INCLUDED_)
#define GPLAT_CONNECTION_H_INCLUDED_

#include <cstring>
#include <ctime>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include "higplat.h"

template<typename T, typename CharT>
T read_value(CharT* buffer) {
	static_assert(std::is_same_v<std::remove_cv_t<CharT>, char>, "buffer must be char*");

	if constexpr (std::is_same_v<T, std::string>) {
		return std::string(buffer);
	}
	else if constexpr (std::is_same_v<T, const char*>) {
		return buffer;
	}
	else if constexpr (std::is_same_v<T, char*>) {
		static_assert(!std::is_const_v<CharT>, "cannot return char* from const char*");
		return buffer;
	}
	else {
		static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
		T result{};
		std::memcpy(&result, buffer, sizeof(T));
		return result;
	}
}

// Base of the exceptions thrown by GplatConnection; code() is the gPlat error code.
class GplatError : public std::runtime_error
{
public:
	GplatError(unsigned int code, const char* message)
		: std::runtime_error(std::string(message ? message : "") + " (Code: " + std::to_string(code) + ")"), m_code(code) {}

	unsigned int code() const noexcept { return m_code; }

private:
	unsigned int m_code;
};

// Caller bug (GPLAT_ERRCAT_USAGE): invalid parameter, size mismatch, buffer too small...
// The connection stays open (a response too large for the buffer has been read and dropped).
class GplatUsageError : public GplatError
{
public:
	using GplatError::GplatError;
};

// The connection is unusable (GPLAT_ERRCAT_CONNECTION: I/O failure, peer closed, malformed response) and has
// already been closed, or it was not open (ERROR_SOCKET_NOT_CONNECTED). Call open() again and re-subscribe.
class GplatConnectionError : public GplatError
{
public:
	using GplatError::GplatError;
};

// C++ wrapper of the gPlat network API (higplat.h is the pure C interface of libhigplat.so).
// Header-only on purpose: std::string and exceptions stay in the caller's translation unit, so no C++ ABI crosses the library boundary.
// Methods return 0 or a GPLAT_ERRCAT_RESULT code (tag not exist, queue empty, wait/response timeout...) and throw
// GplatUsageError / GplatConnectionError for the other categories. Not thread-safe: use one connection per thread.
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
		return call([&](int fd, unsigned int* err) {
			return ::readq(fd, qname.c_str(), record, actsize, err);
		});
	}

	[[nodiscard]] unsigned int writeq(const std::string& qname, const void* record, int actsize)
	{
		return call([&](int fd, unsigned int* err) {
			return ::writeq(fd, qname.c_str(), const_cast<void*>(record), actsize, err);
		});
	}

	[[nodiscard]] unsigned int clearq(const std::string& qname)
	{
		return call([&](int fd, unsigned int* err) {
			return ::clearq(fd, qname.c_str(), err);
		});
	}

	[[nodiscard]] unsigned int peekq(const std::string& qname, int position, void* record, int actsize, RECORD_HEAD* recordhead = nullptr)
	{
		return call([&](int fd, unsigned int* err) {
			return ::peekq(fd, qname.c_str(), position, record, actsize, recordhead, err);
		});
	}

	// ---- Board ----
	[[nodiscard]] unsigned int readb(const std::string& tagname, void* value, int actsize, timespec* timestamp = nullptr)
	{
		return call([&](int fd, unsigned int* err) {
			return ::readb(fd, tagname.c_str(), value, actsize, err, timestamp);
		});
	}

	[[nodiscard]] unsigned int writeb(const std::string& tagname, const void* value, int actsize)
	{
		return call([&](int fd, unsigned int* err) {
			return ::writeb(fd, tagname.c_str(), const_cast<void*>(value), actsize, err);
		});
	}

	[[nodiscard]] unsigned int writeb_notpost(const std::string& tagname, const void* value, int actsize)
	{
		return call([&](int fd, unsigned int* err) {
			return ::writeb_notpost(fd, tagname.c_str(), const_cast<void*>(value), actsize, err);
		});
	}

	[[nodiscard]] unsigned int readb_string(const std::string& tagname, char* value, int buffersize, timespec* timestamp = nullptr)
	{
		return call([&](int fd, unsigned int* err) {
			return ::readb_string(fd, tagname.c_str(), value, buffersize, err, timestamp);
		});
	}

	[[nodiscard]] unsigned int readb_string(const std::string& tagname, std::string& value, timespec* timestamp = nullptr)
	{
		std::string buffer(GPLAT_MAX_DATA_SIZE, '\0');
		unsigned int error = readb_string(tagname, &buffer[0], (int)buffer.size(), timestamp);
		if (error == 0)
			value.assign(buffer.c_str());
		return error;
	}

	[[nodiscard]] unsigned int writeb_string(const std::string& tagname, const char* value)
	{
		return call([&](int fd, unsigned int* err) {
			return ::writeb_string(fd, tagname.c_str(), value, err);
		});
	}

	[[nodiscard]] unsigned int writeb_string(const std::string& tagname, const std::string& value)
	{
		return writeb_string(tagname, value.c_str());
	}

	[[nodiscard]] unsigned int writeb_string_notpost(const std::string& tagname, const char* value)
	{
		return call([&](int fd, unsigned int* err) {
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
		return call([&](int fd, unsigned int* err) {
			return ::subscribe(fd, tagname.c_str(), err);
		});
	}

	[[nodiscard]] unsigned int subscribedelaypost(const std::string& tagname, const std::string& eventname, int delaytime)
	{
		return call([&](int fd, unsigned int* err) {
			return ::subscribedelaypost(fd, tagname.c_str(), eventname.c_str(), delaytime, err);
		});
	}

	// On timeout returns ERROR_WAIT_TIMEOUT and sets tagname to "WAIT_TIMEOUT".
	[[nodiscard]] unsigned int waitpostdata(std::string& tagname, void* value, int buffersize, int timeout)
	{
		char name[GPLAT_TAGNAME_SIZE] = {};
		unsigned int error = call([&](int fd, unsigned int* err) {
			return ::waitpostdata(fd, name, sizeof(name), value, buffersize, timeout, err);
		});
		tagname = name;
		return error;
	}

	// ---- Request/Response ----
	[[nodiscard]] unsigned int getresponse(const std::string& request_tag, const void* request_value, int request_size,
		const std::string& response_tag, void* response_value, int response_size, int timeout_ms = 2000)
	{
		return call([&](int fd, unsigned int* err) {
			return ::getresponse(fd, request_tag.c_str(), const_cast<void*>(request_value), request_size,
				response_tag.c_str(), response_value, response_size, err, timeout_ms);
		});
	}

	// ---- PLC ----
	[[nodiscard]] unsigned int write_plc_string(const std::string& tagname, const std::string& str)
	{
		return call([&](int fd, unsigned int* err) {
			return ::write_plc_string(fd, tagname.c_str(), str.c_str(), err);
		});
	}

	[[nodiscard]] unsigned int write_plc_bool(const std::string& tagname, bool value)
	{
		return call([&](int fd, unsigned int* err) {
			return ::write_plc_bool(fd, tagname.c_str(), value, err);
		});
	}
	template<typename T> unsigned int write_plc_bool(const std::string& tagname, T value) = delete;

	[[nodiscard]] unsigned int write_plc_short(const std::string& tagname, short value)
	{
		return call([&](int fd, unsigned int* err) {
			return ::write_plc_short(fd, tagname.c_str(), value, err);
		});
	}
	template<typename T> unsigned int write_plc_short(const std::string& tagname, T value) = delete;

	[[nodiscard]] unsigned int write_plc_ushort(const std::string& tagname, unsigned short value)
	{
		return call([&](int fd, unsigned int* err) {
			return ::write_plc_ushort(fd, tagname.c_str(), value, err);
		});
	}
	template<typename T> unsigned int write_plc_ushort(const std::string& tagname, T value) = delete;

	[[nodiscard]] unsigned int write_plc_int(const std::string& tagname, int value)
	{
		return call([&](int fd, unsigned int* err) {
			return ::write_plc_int(fd, tagname.c_str(), value, err);
		});
	}
	template<typename T> unsigned int write_plc_int(const std::string& tagname, T value) = delete;

	[[nodiscard]] unsigned int write_plc_uint(const std::string& tagname, unsigned int value)
	{
		return call([&](int fd, unsigned int* err) {
			return ::write_plc_uint(fd, tagname.c_str(), value, err);
		});
	}
	template<typename T> unsigned int write_plc_uint(const std::string& tagname, T value) = delete;

	[[nodiscard]] unsigned int write_plc_float(const std::string& tagname, float value)
	{
		return call([&](int fd, unsigned int* err) {
			return ::write_plc_float(fd, tagname.c_str(), value, err);
		});
	}
	template<typename T> unsigned int write_plc_float(const std::string& tagname, T value) = delete;

private:
	// The C library never closes the fd: on a CONNECTION-category error it has only shut the connection down,
	// so the wrapper closes its own fd before throwing.
	template<typename F>
	unsigned int call(F&& f)
	{
		const char* message = nullptr;
		if (!is_open()) {
			::GetErrorCategory(ERROR_SOCKET_NOT_CONNECTED, &message);
			throw GplatConnectionError(ERROR_SOCKET_NOT_CONNECTED, message);
		}

		unsigned int error = 0;
		if (f(m_sockfd, &error))
			return error;	// non-zero only for waitpostdata timeout
		if (error == 0)
			error = ERROR_SOCKET_NOT_CONNECTED;	// every failure sets a code; treat a missing one as a lost connection

		int category = ::GetErrorCategory(error, &message);
		if (category == GPLAT_ERRCAT_CONNECTION) {
			GplatConnectionError e(error, message);
			close();
			throw e;
		}
		if (category == GPLAT_ERRCAT_USAGE)
			throw GplatUsageError(error, message);
		return error;
	}

	std::string m_server;
	int m_port = 0;
	int m_sockfd = -1;
};

#endif // GPLAT_CONNECTION_H_INCLUDED_
