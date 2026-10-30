// Internal to libhigplat: error categories and error reporting shared by the local and network APIs.
#if !defined(HIGPLAT_ERROR_H_INCLUDED_)
#define HIGPLAT_ERROR_H_INCLUDED_

#include <cstring>

#include "../include/higplat.h"

// Library internals are not exported from libhigplat.so (and calls to them skip the PLT).
#pragma GCC visibility push(hidden)

// Error category decides how callers react; GplatConnection throws for Usage and Connection
enum class ErrorCategory {
    Result = GPLAT_ERRCAT_RESULT,           // outcome the caller branches on: not exist, empty/full, timeout, capacity
    Usage = GPLAT_ERRCAT_USAGE,             // caller bug: bad parameter, size mismatch, buffer too small
    Connection = GPLAT_ERRCAT_CONNECTION    // connection unusable: the network API has shut it down, the caller must disconnectgplat()
};

struct ErrorInfo {
    ErrorCategory category;
    const char* message;
};

// strerror_r is the GNU variant (returns char*) under _GNU_SOURCE, the XSI one (returns int) otherwise
inline const char* StrerrorResult(char* result, char*) { return result; }
inline const char* StrerrorResult(int result, char* buf) { return result == 0 ? buf : "unknown errno"; }

// The returned text stays valid until the next call on the same thread.
inline const char* ErrnoMessage(int err) {
    thread_local char buf[128];
    return StrerrorResult(strerror_r(err, buf, sizeof(buf)), buf);
}

inline ErrorInfo GetErrorInfo(unsigned int errorCode) {
    // the network API stores errno from failed send/recv as the error code
    if (errorCode != 0 && errorCode < MY_ERR_OFFSET)
        return { ErrorCategory::Connection, ErrnoMessage(static_cast<int>(errorCode)) };

    switch (errorCode) {
        case 0:
            return { ErrorCategory::Result, "no error" };

        // ---- Usage ----
        case ERROR_FILENAME_TOO_LONG:
            return { ErrorCategory::Usage, "filename too long" };
        case ERROR_RECORDSIZE:
            return { ErrorCategory::Usage, "record size invalid" };
        case ERROR_STARTPOSITION:
            return { ErrorCategory::Usage, "bad start position" };
        case ERROR_OPERATE_PROHIBIT:
            return { ErrorCategory::Usage, "unsupported operation" };
        case ERROR_TABLE_ROWID:
            return { ErrorCategory::Usage, "table bad row id" };
        case ERROR_PARAMETER_SIZE:
            return { ErrorCategory::Usage, "parameter size invalid" };
        case STRING_TOO_LONG:
            return { ErrorCategory::Usage, "string too long" };
        case BUFFER_TOO_SMALL:
            return { ErrorCategory::Usage, "string longer than buffer" };
        case ERROR_INVALID_PARAMETER:
            return { ErrorCategory::Usage, "invalid parameter" };
        case ERROR_BUFFER_TOO_SMALL:
            return { ErrorCategory::Usage, "data larger than buffer" };

        // ---- Connection ----
        case ERROR_SOCKET_NOT_CONNECTED:
            return { ErrorCategory::Connection, "socket not connected" };
        case ERROR_INVALID_RESPONSE:
            return { ErrorCategory::Connection, "invalid response" };

        // ---- Result ----
        case ERROR_DQ_EMPTY:
            return { ErrorCategory::Result, "queue empty" };
        case ERROR_DQ_FULL:
            return { ErrorCategory::Result, "queue full" };
        case ERROR_FILE_IN_USE:
            return { ErrorCategory::Result, "file already in use" };
        case ERROR_FILE_CREATE_FAILSURE:
            return { ErrorCategory::Result, "failed to create file" };
        case ERROR_TABLE_OVERFLOW:
            return { ErrorCategory::Result, "table overflow" };
        case ERROR_RECORD_NOT_EXIST:
            return { ErrorCategory::Result, "record not exist" };
        case ERROR_ALREADY_LOAD:
            return { ErrorCategory::Result, "queue already loaded" };
        case ERROR_NO_SPACE:
            return { ErrorCategory::Result, "no space" };
        case ERROR_TABLE_NOT_EXIST:
            return { ErrorCategory::Result, "table not exist" };
        case ERROR_TABLE_ALREADY_EXIST:
            return { ErrorCategory::Result, "table already exist" };
        case ERROR_ITEM_NOT_EXIST:
            return { ErrorCategory::Result, "item not exist" };
        case ERROR_ITEM_ALREADY_EXIST:
            return { ErrorCategory::Result, "item already exist" };
        case ERROR_ITEM_OVERFLOW:
            return { ErrorCategory::Result, "item overflow" };
        case ERROR_TAG_NOT_EXIST:
            return { ErrorCategory::Result, "tag not exist" };
        case ERROR_WAIT_TIMEOUT:
            return { ErrorCategory::Result, "wait post timeout" };
        case ERROR_RESPONSE_TIMEOUT:
            return { ErrorCategory::Result, "wait response timeout" };
        case ERROR_REQUEST_QUEUE_FULL:
            return { ErrorCategory::Result, "request queue full" };

        // no longer produced
        case ERROR_DQFILE_NOT_FOUND:
        case ERROR_DQ_NOT_OPEN:
        case ERROR_FILE_OPEN_FAILSURE:
        case ERROR_CREATE_FILEMAPPINGOBJECT:
        case ERROR_OPEN_FILEMAPPINGOBJECT:
        case ERROR_MAPVIEWOFFILE:
        case ERROR_CREATE_MUTEX:
        case ERROR_OPEN_MUTEX:
        case ERROR_RECORD_ALREAD_EXIST:
        case ERROR_ALREADY_OPEN:
        case ERROR_ALREADY_CLOSE:
        case ERROR_ALREADY_UNLOAD:
        case ERROR_MSGSIZE:
        case ERROR_BUFFER_SIZE:
        case CODE_QEMPTY:
        case CODE_QFULL:
            return { ErrorCategory::Result, "deprecated error code" };
        default:
            return { ErrorCategory::Result, "unknown error" };
    }
}

// Last error of the local API on this thread, returned by GetLastErrorQ()
extern thread_local unsigned int g_lastError;

// Calls the error hook once; message == nullptr means the standard description of the code
void ReportError(unsigned int error, const char* func, const char* message);

// Declared once at the top of every exported network function (never in internal helpers),
// so each API call reports a non-zero error code to the hook exactly once.
class AutoErrorCheck {
public:
    AutoErrorCheck(unsigned int* error, const char* func) noexcept : m_error(error), m_func(func) {}

    // The C API must not throw; GplatConnection turns Usage/Connection codes into exceptions.
    ~AutoErrorCheck() {
        if (m_error != nullptr && *m_error != 0)
            ReportError(*m_error, m_func, nullptr);
    }

    AutoErrorCheck(const AutoErrorCheck&) = delete;
    AutoErrorCheck& operator=(const AutoErrorCheck&) = delete;

private:
    unsigned int* m_error;
    const char* m_func;
};

#pragma GCC visibility pop

#endif // !defined(HIGPLAT_ERROR_H_INCLUDED_)
