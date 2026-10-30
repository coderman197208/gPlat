// Error reporting: the error hook of the network API and GetLastErrorQ of the local API.
#include <cerrno>
#include <cstdio>
#include <mutex>

#include "error.h"

thread_local unsigned int g_lastError = 0;

namespace {

void DefaultErrorHook(unsigned int error, int category, const char* message, const char* func, void*)
{
	const char* label = category == GPLAT_ERRCAT_USAGE ? "usage"
		: category == GPLAT_ERRCAT_CONNECTION ? "connection" : nullptr;
	if (label == nullptr)
		return;
	char line[256];
	snprintf(line, sizeof(line), "[higplat %s] %s: %s (code %u)\n", label, func, message, error);
	fputs(line, stderr);	// one call per line so concurrent threads don't interleave
}

std::mutex g_errorHookMutex;
GPLAT_ERROR_HOOK g_errorHook = DefaultErrorHook;
void* g_errorHookUser = nullptr;

} // namespace

void ReportError(unsigned int error, const char* func, const char* message)
{
	const int savedErrno = errno;
	GPLAT_ERROR_HOOK hook;
	void* user;
	{
		std::lock_guard<std::mutex> lock(g_errorHookMutex);
		hook = g_errorHook;
		user = g_errorHookUser;
	}
	if (hook != nullptr) {
		const ErrorInfo info = GetErrorInfo(error);
		hook(error, static_cast<int>(info.category), message ? message : info.message, func, user);
	}
	errno = savedErrno;
}

extern "C" void SetErrorHook(GPLAT_ERROR_HOOK hook, void* user)
{
	std::lock_guard<std::mutex> lock(g_errorHookMutex);
	g_errorHook = hook;
	g_errorHookUser = user;
}

extern "C" int GetErrorCategory(unsigned int error, const char** message)
{
	const ErrorInfo info = GetErrorInfo(error);
	if (message)
		*message = info.message;
	return static_cast<int>(info.category);
}

extern "C" bool IsFatalError(unsigned int error, const char** message)
{
	return GetErrorCategory(error, message) == GPLAT_ERRCAT_USAGE;
}

extern "C" unsigned int GetLastErrorQ()
{
	return g_lastError;
}
