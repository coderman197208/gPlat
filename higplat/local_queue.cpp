// Local queue API: direct access to the mmap'd queue files loaded by this process.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "qbd.h"

namespace {

constexpr size_t kIpSize = sizeof(RECORD_HEAD::remoteIp);

// Head of a loaded queue, or nullptr with g_lastError set
QUEUE_HEAD* FetchQueue(const char* name, TABLE_MSG& tab)
{
	if (!fetchtab(name, tab))
		return nullptr;
	auto* head = static_cast<QUEUE_HEAD*>(tab.lpMapAddress);
	if (head->qbdtype != QUEUE_T) {
		g_lastError = ERROR_OPERATE_PROHIBIT;
		return nullptr;
	}
	return head;
}

// A read needs room for a whole record: ASCII buffers may be larger, binary ones must match
bool ReadSizeOk(const QUEUE_HEAD* head, int actSize)
{
	return !((head->dataType == ASCII_TYPE && actSize < head->size)
		|| (head->dataType == BINARY_TYPE && actSize != head->size));
}

void CopyIp(char* dst, const char* src)
{
	const size_t len = strnlen(src, kIpSize - 1);
	memcpy(dst, src, len);
	dst[len] = '\0';
}

// Copies the data of the record (and its sender IP when remoteIp != nullptr)
void CopyRecordOut(QUEUE_HEAD* head, int slot, void* lpRecord, char* remoteIp)
{
	const char* record = QueueRecord(head, slot);
	if (remoteIp != nullptr)
		CopyIp(remoteIp, reinterpret_cast<const RECORD_HEAD*>(record)->remoteIp);
	memcpy(lpRecord, record + RECORDHEADSIZE, head->size);
}

// Number of records MulReadQ copies from start, at most count; caller holds the queue lock
int MulReadCount(const QUEUE_HEAD* head, int start, int count)
{
	const int available = (head->operateMode == NORMAL_MODE) ? head->num : head->num - start;
	return std::min(available, count);
}

// Copies readCount records including their RECORD_HEAD: a normal queue slot by slot from start,
// a shift queue from the start-th latest record backwards. Caller holds the queue lock
void MulReadCopy(QUEUE_HEAD* head, int start, int readCount, char* dst)
{
	const int num = head->num;
	const size_t recordSize = head->size + RECORDHEADSIZE;
	const bool normal = (head->operateMode == NORMAL_MODE);
	const int first = normal ? start : (head->writePoint + num - start) % num;
	for (int i = 0; i < readCount; i++) {
		const int slot = normal ? (first + i) % num : (first + num - i) % num;
		memcpy(dst + i * recordSize, QueueRecord(head, slot), recordSize);
	}
}

// 队列名来自网络，只允许 QBD 目录下的普通文件名
bool IsValidQueueName(const char* name)
{
	const size_t len = strnlen(name, MAXDQNAMELENTH);
	return len > 0 && len < MAXDQNAMELENTH
		&& strchr(name, '/') == nullptr
		&& strcmp(name, ".") != 0 && strcmp(name, "..") != 0
		&& strcmp(name, "_qbd_file_directory") != 0;
}

} // namespace

// dateType: ASCII_TYPE / BINARY_TYPE; operateMode: SHIFT_MODE / NORMAL_MODE
extern "C" bool CreateQ(const char* lpFileName, int recordSize, int recordNum, int dateType, int operateMode, void* pType, int typeSize)
{
	if (recordSize > GPLAT_MAX_DATA_SIZE || typeSize > TYPEMAXSIZE) {
		g_lastError = ERROR_PARAMETER_SIZE;
		return false;
	}

	const long fileSize = recordNum * (recordSize + RECORDHEADSIZE) + QUEUEHEADSIZE + TYPEMAXSIZE;
	return CreateQbdFile(lpFileName, fileSize, [&](char* base) {
		auto* head = reinterpret_cast<QUEUE_HEAD*>(base);
		head->qbdtype = QUEUE_T;
		head->dataType = dateType;
		head->operateMode = operateMode;
		head->num = recordNum;
		head->size = recordSize;
		head->writePoint = recordNum - 1;
		head->readPoint = (operateMode == NORMAL_MODE) ? (recordNum - 1) : 0;
		FormatLocalTime(head->createDate);
		if (pType != nullptr && typeSize > 0 && typeSize < TYPEMAXSIZE) {
			memcpy(QueueRecord(head, recordNum), pType, typeSize);
			head->typesize = typeSize;
		}
		for (int i = 0; i < recordNum; i++) {
			auto* recordHead = reinterpret_cast<RECORD_HEAD*>(QueueRecord(head, i));
			recordHead->ack = 0;
			recordHead->index = i;
		}
	});
}

// 供服务端运行时创建队列：不覆盖已载入的同名对象（包括 BOARD），创建成功后立即载入
extern "C" bool CreateAndLoadQ(const char* lpFileName, int recordSize, int recordNum, int dataType, int operateMode, void* pType, int typeSize)
{
	if (!IsValidQueueName(lpFileName)) {
		g_lastError = ERROR_INVALID_PARAMETER;
		return false;
	}

	// 检查、创建、载入必须作为整体执行，否则并发创建同名队列会删掉已载入的文件
	static std::mutex createMutex;
	std::lock_guard<std::mutex> createLock(createMutex);
	if (IsLoaded(lpFileName)) {
		g_lastError = ERROR_ALREADY_LOAD;
		return false;
	}

	if (!CreateQ(lpFileName, recordSize, recordNum, dataType, operateMode, pType, typeSize))
		return false;
	if (!LoadQ(lpFileName)) {
		g_lastError = ERROR_FILE_OPEN_FAILSURE;
		return false;
	}
	return true;
}

extern "C" bool ReadHead(const char* lpDqName, void* lpHead)
{
	TABLE_MSG tab;
	QUEUE_HEAD* head = FetchQueue(lpDqName, tab);
	if (head == nullptr)
		return false;

	std::lock_guard<std::mutex> lock(*tab.hMutex);
	memcpy(lpHead, head, QUEUEHEADSIZE);
	return true;
}

// Removes a record (a shift queue returns its latest record, without removing it)
extern "C" bool ReadQ(const char* lpDqName, void* lpRecord, int actSize, char* remoteIp)
{
	TABLE_MSG tab;
	QUEUE_HEAD* head = FetchQueue(lpDqName, tab);
	if (head == nullptr)
		return false;

	if (!ReadSizeOk(head, actSize)) {
		g_lastError = ERROR_RECORDSIZE;
		return false;
	}

	std::lock_guard<std::mutex> lock(*tab.hMutex);
	if (head->operateMode == NORMAL_MODE) {
		if (head->readPoint != head->writePoint) {
			head->readPoint = (head->readPoint + 1) % head->num;
			CopyRecordOut(head, head->readPoint, lpRecord, remoteIp);
			return true;
		}
	}
	else if (head->readPoint != 0) {
		CopyRecordOut(head, head->writePoint, lpRecord, remoteIp);
		return true;
	}

	g_lastError = ERROR_DQ_EMPTY;
	return false;
}

// 从队列中删除一条记录（读指针加1），不返回数据；移位队列不支持
extern "C" bool PopJustRecordFromQueue(const char* lpDqName)
{
	TABLE_MSG tab;
	QUEUE_HEAD* head = FetchQueue(lpDqName, tab);
	if (head == nullptr)
		return false;

	std::lock_guard<std::mutex> lock(*tab.hMutex);
	if (head->operateMode != NORMAL_MODE) {
		g_lastError = 0;
		return false;
	}
	if (head->readPoint == head->writePoint) {
		g_lastError = ERROR_DQ_EMPTY;
		return false;
	}
	head->readPoint = (head->readPoint + 1) % head->num;
	return true;
}

// Copies up to *count records (with RECORD_HEAD) from start into lpRecord without removing them
extern "C" bool MulReadQ(const char* lpDqName, void* lpRecord, int start, int* count, int* pRecordSize)
{
	TABLE_MSG tab;
	QUEUE_HEAD* head = FetchQueue(lpDqName, tab);
	if (head == nullptr)
		return false;

	if (pRecordSize != nullptr)
		*pRecordSize = head->size;
	if (start > head->num - 1) {
		g_lastError = ERROR_STARTPOSITION;
		return false;
	}

	std::lock_guard<std::mutex> lock(*tab.hMutex);
	*count = MulReadCount(head, start, *count);
	MulReadCopy(head, start, *count, static_cast<char*>(lpRecord));
	return true;
}

// Like MulReadQ into a malloc'd buffer returned in *lppRecords, which the caller frees
extern "C" bool MulReadQ2(const char* lpDqName, void** lppRecords, int start, int* count, int* pRecordSize)
{
	TABLE_MSG tab;
	QUEUE_HEAD* head = FetchQueue(lpDqName, tab);
	if (head == nullptr)
		return false;

	if (pRecordSize != nullptr)
		*pRecordSize = head->size;
	if (start > head->num - 1) {
		g_lastError = ERROR_STARTPOSITION;
		*lppRecords = nullptr;
		*count = 0;
		if (pRecordSize != nullptr)
			*pRecordSize = 0;
		return false;
	}

	std::lock_guard<std::mutex> lock(*tab.hMutex);
	const int readCount = MulReadCount(head, start, *count);
	*count = readCount;
	void* buff = malloc(readCount * (head->size + RECORDHEADSIZE));
	if (buff == nullptr) {
		*lppRecords = nullptr;
		*count = 0;
		return false;
	}
	MulReadCopy(head, start, readCount, static_cast<char*>(buff));
	*lppRecords = buff;
	return true;
}

// actSize == 0 on an ASCII queue writes the string including its '\0'
extern "C" bool WriteQ(const char* lpDqName, void* lpRecord, int actSize, const char* remoteIp)
{
	TABLE_MSG tab;
	QUEUE_HEAD* head = FetchQueue(lpDqName, tab);
	if (head == nullptr)
		return false;

	if (head->dataType == ASCII_TYPE && actSize == 0)
		actSize = static_cast<int>(strlen(static_cast<const char*>(lpRecord)) + 1);

	if (actSize < 0
		|| (head->dataType == ASCII_TYPE && actSize > head->size)
		|| (head->dataType == BINARY_TYPE && actSize != head->size)) {
		g_lastError = ERROR_RECORDSIZE;
		return false;
	}

	std::lock_guard<std::mutex> lock(*tab.hMutex);
	if (head->operateMode == NORMAL_MODE && (head->writePoint + 1) % head->num == head->readPoint) {
		g_lastError = ERROR_DQ_FULL;
		return false;
	}

	head->writePoint = (head->writePoint + 1) % head->num;
	char* record = QueueRecord(head, head->writePoint);
	auto* recordHead = reinterpret_cast<RECORD_HEAD*>(record);
	FormatLocalTime(recordHead->createDate);
	recordHead->ack = 0;
	CopyIp(recordHead->remoteIp, remoteIp != nullptr ? remoteIp : "127.0.0.1");
	memcpy(record + RECORDHEADSIZE, lpRecord, actSize);
	if (head->operateMode == SHIFT_MODE)
		head->readPoint = 1;
	return true;
}

extern "C" bool ClearQ(const char* lpDqName)
{
	TABLE_MSG tab;
	QUEUE_HEAD* head = FetchQueue(lpDqName, tab);
	if (head == nullptr)
		return false;

	std::lock_guard<std::mutex> lock(*tab.hMutex);
	if (head->operateMode == NORMAL_MODE)
		head->readPoint = head->writePoint = head->num - 1;
	else
		head->readPoint = 0;
	return true;
}

// Sets the read and/or write pointer; -1 leaves that pointer unchanged
extern "C" bool SetPtrQ(const char* lpDqName, int readPtr, int writePtr)
{
	TABLE_MSG tab;
	QUEUE_HEAD* head = FetchQueue(lpDqName, tab);
	if (head == nullptr)
		return false;

	const int num = head->num;
	const auto inRange = [num](int ptr) { return ptr >= 0 && ptr < num; };
	const bool setRead = inRange(readPtr) && (inRange(writePtr) || writePtr == -1);
	const bool setWrite = inRange(writePtr) && (inRange(readPtr) || readPtr == -1);
	if (!setRead && !setWrite)
		return false;

	std::lock_guard<std::mutex> lock(*tab.hMutex);
	if (setRead)
		head->readPoint = readPtr;
	if (setWrite)
		head->writePoint = writePtr;
	return true;
}

// Reads the next record without removing it; not supported by shift queues
extern "C" bool PeekQ(const char* lpDqName, void* lpRecord, int actSize)
{
	TABLE_MSG tab;
	QUEUE_HEAD* head = FetchQueue(lpDqName, tab);
	if (head == nullptr)
		return false;

	if (!ReadSizeOk(head, actSize)) {
		g_lastError = ERROR_RECORDSIZE;
		return false;
	}
	if (head->operateMode == SHIFT_MODE) {
		g_lastError = ERROR_OPERATE_PROHIBIT;
		return false;
	}

	std::lock_guard<std::mutex> lock(*tab.hMutex);
	if (head->readPoint == head->writePoint) {
		g_lastError = ERROR_DQ_EMPTY;
		return false;
	}
	CopyRecordOut(head, (head->readPoint + 1) % head->num, lpRecord, nullptr);
	return true;
}

// 不移动读写指针地读取一条记录，position 为 PEEK_NEXT 或 PEEK_LATEST；lpRecordHead 可为空
extern "C" bool PeekQRecord(const char* lpDqName, int position, void* lpRecord, int actSize, RECORD_HEAD* lpRecordHead)
{
	if (position != PEEK_NEXT && position != PEEK_LATEST) {
		g_lastError = ERROR_INVALID_PARAMETER;
		return false;
	}

	TABLE_MSG tab;
	QUEUE_HEAD* head = FetchQueue(lpDqName, tab);
	if (head == nullptr)
		return false;

	if (!ReadSizeOk(head, actSize)) {
		g_lastError = ERROR_RECORDSIZE;
		return false;
	}

	std::lock_guard<std::mutex> lock(*tab.hMutex);
	int slot;
	bool empty;
	if (head->operateMode == NORMAL_MODE) {
		empty = (head->readPoint == head->writePoint);
		slot = (position == PEEK_NEXT) ? (head->readPoint + 1) % head->num : head->writePoint;
	}
	else {
		// 移位队列的 readq 总是返回最新记录，readPoint 仅标记是否写入过
		empty = (head->readPoint == 0);
		slot = head->writePoint;
	}
	if (empty) {
		g_lastError = ERROR_DQ_EMPTY;
		return false;
	}

	if (lpRecordHead != nullptr)
		memcpy(lpRecordHead, QueueRecord(head, slot), RECORDHEADSIZE);
	CopyRecordOut(head, slot, lpRecord, nullptr);
	return true;
}

// 列出已载入的队列名，以 '\0' 分隔依次存入 names，*namesSize 为使用的总字节数
extern "C" bool ListQ(char* names, int buffSize, int* namesSize, int* count)
{
	*namesSize = 0;
	*count = 0;
	return ForEachLoaded([&](const TABLE_MSG& entry) {
		if (*static_cast<const int*>(entry.lpMapAddress) != QUEUE_T)
			return true;

		const int len = static_cast<int>(strlen(entry.dqname)) + 1;
		if (*namesSize + len > buffSize) {
			g_lastError = ERROR_BUFFER_TOO_SMALL;
			return false;
		}
		memcpy(names + *namesSize, entry.dqname, len);
		*namesSize += len;
		(*count)++;
		return true;
	});
}

extern "C" bool IsEmptyQ(const char* lpDqName)
{
	TABLE_MSG tab;
	QUEUE_HEAD* head = FetchQueue(lpDqName, tab);
	if (head == nullptr)
		return false;

	std::lock_guard<std::mutex> lock(*tab.hMutex);
	if (head->operateMode == NORMAL_MODE)
		return head->readPoint == head->writePoint;
	return head->readPoint == 0;
}

// A shift queue is never full
extern "C" bool IsFullQ(const char* lpDqName)
{
	TABLE_MSG tab;
	QUEUE_HEAD* head = FetchQueue(lpDqName, tab);
	if (head == nullptr)
		return false;

	std::lock_guard<std::mutex> lock(*tab.hMutex);
	return head->operateMode == NORMAL_MODE && (head->writePoint + 1) % head->num == head->readPoint;
}
