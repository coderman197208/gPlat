// Table of the QBD files loaded by this process, and the file lifecycle: create / load / unload / flush.
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <new>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "qbd.h"

namespace fs = std::filesystem;

namespace {

char g_qbdPath[100] = "../qbdfile/";

// Copies path into g_qbdPath, making sure it ends with '/'
void AssignQbdPath(const char* path)
{
	if (path == nullptr || path[0] == '\0')
		return;
	strncpy(g_qbdPath, path, sizeof(g_qbdPath) - 2);
	g_qbdPath[sizeof(g_qbdPath) - 2] = '\0';
	const size_t len = strlen(g_qbdPath);
	if (len > 0 && g_qbdPath[len - 1] != '/') {
		g_qbdPath[len] = '/';
		g_qbdPath[len + 1] = '\0';
	}
}

// 从环境变量 GPLAT_QBD_PATH 初始化 QBD 目录，共享库加载时自动执行
__attribute__((constructor)) void InitQbdPathFromEnv()
{
	AssignQbdPath(getenv("GPLAT_QBD_PATH"));
}

// Open-addressing hash table (double hashing) of the loaded files, guarded by g_tableMutex
TABLE_MSG g_table[TABLESIZE];
int g_tableUsed = 0;	// slots ever used
std::mutex g_tableMutex;

// Slot of a loaded file, or -1; caller holds g_tableMutex
int FindSlot(const char* dqname)
{
	int loc = hash1(dqname);
	const int c = hash2(dqname);
	while (g_table[loc].dqname[0] != '\0' && strcmp(g_table[loc].dqname, dqname) != 0)
		loc = (loc + c) % TABLESIZE;
	return (g_table[loc].dqname[0] == '\0' || g_table[loc].erased) ? -1 : loc;
}

// Caller holds g_tableMutex
bool InsertSlot(const TABLE_MSG& tabmsg)
{
	int loc = hash1(tabmsg.dqname);
	const int c = hash2(tabmsg.dqname);
	while (g_table[loc].dqname[0] != '\0' && !g_table[loc].erased)
		loc = (loc + c) % TABLESIZE;

	TABLE_MSG& slot = g_table[loc];
	if (!slot.erased) {
		// keep one slot never used so that probing always terminates
		if (g_tableUsed == TABLESIZE - 1) {
			g_lastError = ERROR_TABLE_OVERFLOW;
			return false;
		}
		g_tableUsed++;
	}
	const int count = slot.count;
	slot = tabmsg;
	slot.erased = false;
	slot.count = count + 1;
	return true;
}

// Drops one reference; when it was the last, frees the slot, returns its entry and true. Caller holds g_tableMutex
bool ReleaseSlot(const char* dqname, TABLE_MSG& tabmsg)
{
	const int loc = FindSlot(dqname);
	if (loc < 0) {
		g_lastError = ERROR_RECORD_NOT_EXIST;
		return false;
	}
	TABLE_MSG& slot = g_table[loc];
	if (--slot.count >= 1)
		return false;

	tabmsg = slot;
	strcpy(slot.dqname, kErasedName);
	slot.erased = true;
	slot.count = 0;
	return true;
}

} // namespace

bool fetchtab(const char* dqname, TABLE_MSG& tabmsg)
{
	std::lock_guard<std::mutex> lock(g_tableMutex);
	const int loc = FindSlot(dqname);
	if (loc < 0) {
		g_lastError = ERROR_RECORD_NOT_EXIST;
		return false;
	}
	tabmsg = g_table[loc];
	return true;
}

bool IsLoaded(const char* dqname)
{
	std::lock_guard<std::mutex> lock(g_tableMutex);
	return FindSlot(dqname) >= 0;
}

bool ForEachLoaded(const std::function<bool(const TABLE_MSG&)>& visit)
{
	std::lock_guard<std::mutex> lock(g_tableMutex);
	for (const TABLE_MSG& entry : g_table) {
		if (entry.dqname[0] == '\0' || entry.erased)
			continue;
		if (!visit(entry))
			return false;
	}
	return true;
}

std::string QbdFilePath(const char* name)
{
	return std::string(g_qbdPath) + name;
}

void FormatLocalTime(char* buf)
{
	const time_t now = time(nullptr);
	tm local{};
	localtime_r(&now, &local);
	strftime(buf, 20, "%Y-%m-%d %H:%M:%S", &local);
}

bool CreateQbdFile(const char* name, long fileSize, const std::function<void(char* base)>& init)
{
	// the name must fit TABLE_MSG::dqname, or LoadQ couldn't load the file
	if (strlen(name) >= MAXDQNAMELENTH) {
		g_lastError = ERROR_FILENAME_TOO_LONG;
		return false;
	}

	const fs::path path = QbdFilePath(name);
	std::error_code ec;

	// 先查找同名文件删除之
	if (fs::exists(path, ec) && !fs::remove(path, ec)) {
		g_lastError = ERROR_FILE_IN_USE;
		return false;
	}

	if (path.has_parent_path() && !fs::exists(path.parent_path(), ec)
		&& !fs::create_directories(path.parent_path(), ec)) {
		g_lastError = ERROR_FILE_CREATE_FAILSURE;
		return false;
	}

	const int fd = open(path.c_str(), O_CREAT | O_RDWR, 0644);
	if (fd == -1) {
		std::cerr << "打开/创建文件失败: " << ErrnoMessage(errno) << std::endl;
		return false;
	}

	if (ftruncate(fd, fileSize) == -1) {
		std::cerr << "调整文件大小失败: " << ErrnoMessage(errno) << std::endl;
		close(fd);
		return false;
	}

	void* base = mmap(nullptr, fileSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (base == MAP_FAILED) {
		std::cerr << "内存映射失败: " << ErrnoMessage(errno) << std::endl;
		close(fd);
		return false;
	}

	memset(base, 0, fileSize);
	init(static_cast<char*>(base));

	if (msync(base, fileSize, MS_SYNC) == -1) {
		std::cerr << "同步到磁盘失败: " << ErrnoMessage(errno) << std::endl;
	}
	munmap(base, fileSize);
	close(fd);
	return true;
}

extern "C" void SetQbdPath(const char* path)
{
	AssignQbdPath(path);
}

extern "C" bool LoadQ(const char* lpDqName)
{
	if (strlen(lpDqName) >= MAXDQNAMELENTH) {
		g_lastError = ERROR_FILENAME_TOO_LONG;
		return false;
	}

	// 服务端运行时也会载入新建的队列，整个过程持有表锁，避免与 fetchtab 并发访问哈希表
	std::lock_guard<std::mutex> lock(g_tableMutex);
	if (FindSlot(lpDqName) >= 0) {
		g_lastError = ERROR_ALREADY_LOAD;
		return true;
	}

	const std::string path = QbdFilePath(lpDqName);
	const int fd = open(path.c_str(), O_CREAT | O_RDWR, 0644);
	if (fd == -1) {
		std::cerr << "打开文件失败: " << ErrnoMessage(errno) << std::endl;
		return false;
	}

	struct stat sb;
	if (fstat(fd, &sb) == -1) {
		std::cerr << "获取文件大小失败: " << ErrnoMessage(errno) << std::endl;
		close(fd);
		return false;
	}
	const off_t fileSize = sb.st_size;

	void* base = mmap(nullptr, fileSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (base == MAP_FAILED) {
		std::cerr << "内存映射失败: " << ErrnoMessage(errno) << std::endl;
		close(fd);
		return false;
	}

	TABLE_MSG tabmsg;
	if (*static_cast<int*>(base) == BOARD_T) {
		// the board's locks live in the file: construct them anew on every load
		auto* head = static_cast<BOARD_HEAD*>(base);
		tabmsg.pmutex_rw = new (&head->mutex_rw) std::mutex();
		for (std::mutex& stripe : head->mutex_rw_tag)
			new (&stripe) std::mutex();
	}
	strcpy(tabmsg.dqname, lpDqName);
	tabmsg.hFile = fd;
	tabmsg.lpMapAddress = base;		// FlushQFile 定期把映射写回文件
	tabmsg.hMutex = new std::mutex();
	tabmsg.filesize = fileSize;
	if (InsertSlot(tabmsg))
		return true;

	delete tabmsg.hMutex;
	munmap(base, fileSize);
	close(fd);
	return false;
}

// The mapping stays in place: other threads may still use entries they fetched
extern "C" bool UnloadQ(const char* lpDqName)
{
	std::lock_guard<std::mutex> lock(g_tableMutex);
	TABLE_MSG tabmsg;
	if (!ReleaseSlot(lpDqName, tabmsg))
		return true;

	close(tabmsg.hFile);
	delete tabmsg.hMutex;
	if (tabmsg.pmutex_rw != nullptr) {
		tabmsg.pmutex_rw->~mutex();
		auto* head = static_cast<BOARD_HEAD*>(tabmsg.lpMapAddress);
		for (std::mutex& stripe : head->mutex_rw_tag)
			stripe.~mutex();
	}
	return true;
}

// Unloads every queue and board loaded by this process
extern "C" bool UnloadAll(void)
{
	std::lock_guard<std::mutex> lock(g_tableMutex);
	for (TABLE_MSG& entry : g_table) {
		if (entry.dqname[0] == '\0' || entry.erased)
			continue;
		close(entry.hFile);
		delete entry.hMutex;
		entry.hMutex = nullptr;
		entry.dqname[0] = '\0';
		if (entry.pmutex_rw != nullptr) {
			entry.pmutex_rw->~mutex();
			entry.pmutex_rw = nullptr;
		}
	}
	return true;
}

// 将内存文件映射数据写回文件
extern "C" bool FlushQFile(void)
{
	// msync can be slow: don't hold the table lock while syncing
	std::vector<TABLE_MSG> loaded;
	ForEachLoaded([&loaded](const TABLE_MSG& entry) {
		loaded.push_back(entry);
		return true;
	});

	for (const TABLE_MSG& entry : loaded) {
		if (entry.lpMapAddress != nullptr && msync(entry.lpMapAddress, entry.filesize, MS_SYNC) == -1) {
			std::cerr << entry.dqname << " msync failed: " << ErrnoMessage(errno) << std::endl;
		}
	}
	return true;
}

// Type descriptor of a queue (lpItemName unused) or of a board item
extern "C" bool ReadType(const char* lpDqName, const char* lpItemName, void* inBuff, int buffSize, int* pTypeSize)
{
	*pTypeSize = 0;
	TABLE_MSG tabmsg;
	if (!fetchtab(lpDqName, tabmsg))
		return false;

	const int qbdtype = *static_cast<int*>(tabmsg.lpMapAddress);
	if (qbdtype == QUEUE_T) {
		auto* head = static_cast<QUEUE_HEAD*>(tabmsg.lpMapAddress);
		if (head->typesize > buffSize) {
			g_lastError = ERROR_BUFFER_TOO_SMALL;
			return false;
		}
		memcpy(inBuff, QueueRecord(head, head->num), head->typesize);
		*pTypeSize = head->typesize;
	}
	else if (qbdtype == BOARD_T) {
		auto* head = static_cast<BOARD_HEAD*>(tabmsg.lpMapAddress);
		// DeleteItem moves the type area under the index lock
		std::lock_guard<std::mutex> lock(*tabmsg.pmutex_rw);
		const int loc = FindBoardItem(head->index, lpItemName);
		if (loc < 0 || head->index[loc].typesize == 0)
			return false;

		const BOARD_INDEX_STRUCT& item = head->index[loc];
		if (item.typesize > buffSize) {
			g_lastError = ERROR_BUFFER_TOO_SMALL;
			return false;
		}
		memcpy(inBuff, BoardTypeArea(head) + item.typeaddr, item.typesize);
		*pTypeSize = item.typesize;
	}
	return true;
}

// 演示用导出（include/hello.h）
extern "C" int add(int a, int b)
{
	return a + b;
}
