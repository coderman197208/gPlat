// Internal to libhigplat: layout of QBD files (queue / board) and the table of files loaded by this process.
// The layout is persisted in the mmap'd files; the static_asserts below guard it.
#if !defined(QBD_H_INCLUDED_)
#define QBD_H_INCLUDED_

#include <cstddef>
#include <cstring>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>

#include "../include/higplat.h"
#include "error.h"

#pragma GCC visibility push(hidden)

constexpr int MAXDQNAMELENTH = 40;		// name buffer size including '\0'
static_assert(MAXDQNAMELENTH == GPLAT_TAGNAME_SIZE, "tag name size mismatch");

constexpr std::size_t QUEUEHEADSIZE = sizeof(QUEUE_HEAD);
constexpr std::size_t RECORDHEADSIZE = sizeof(RECORD_HEAD);

enum QbdType : int {
	QUEUE_T,
	BOARD_T,
	DATABASE_T
};

constexpr int MUTEXSIZE = 64;		// stripe locks per board, must be a power of 2
constexpr int TABLESIZE = 277;		// prime; size of the loaded-file table and modulus of hash1/hash2
constexpr int INDEXSIZE = 7177;		// prime; slots of the board index
constexpr int TYPEMAXSIZE = 2048;	// max serialized type descriptor size
constexpr int TYPEAVGSIZE = 32;		// average serialized type descriptor size, sizes the board type area
static_assert((MUTEXSIZE & (MUTEXSIZE - 1)) == 0, "MUTEXSIZE must be a power of 2");

// Name left in a freed slot (loaded-file table, board index): non-empty, so probe chains continue through it
constexpr char kErasedName[] = "CT0D9GG_~$59";

#pragma pack(push, 8)

// A file loaded by this process; copies returned by fetchtab share the mapping and the locks
struct TABLE_MSG
{
	char dqname[MAXDQNAMELENTH] = {};
	int hFile = -1;
	void* lpMapAddress = nullptr;
	std::mutex* hMutex = nullptr;		// queue lock, owned by the table
	std::mutex* pmutex_rw = nullptr;	// board index lock, placement-new'd in the mapped file; nullptr for queues
	bool erased = false;
	int count = 0;						// load reference count
	long filesize = 0;
};

struct BOARD_INDEX_STRUCT
{
	char  itemname[MAXDQNAMELENTH];
	int    startpos;		// reference to the beginning of date part.
	int    itemsize;
	int    strlenth;		// 字符串长度(不包括'\0')
	bool   erased;			// 表删除标志
	timespec timestamp;		// write time
	int	   typeaddr;		// 类型起始地址
	int	   typesize;		// 类型序列化长度
};

struct BOARD_HEAD
{
	int qbdtype;
	int counter;
	int totalsize;		// BOARD_HEAD和后面数据区大小的和，不包括最后面的类型区
	int typesize;		// 最后面的类型区的大小
	int nextpos;		// reference to the beginning of unused date part.
	int nexttypepos;	// reference to the beginning of unused type part.
	int remain;
	int typeremain;		// 类型区剩余大小
	int indexcount;
	std::mutex mutex_rw;					// index lock: hash index traversal and layout changes
	std::mutex mutex_rw_tag[MUTEXSIZE];		// stripe locks (by index slot) for item data
	BOARD_INDEX_STRUCT index[INDEXSIZE];
};

#pragma pack(pop)

static_assert(sizeof(QUEUE_HEAD) == 56 && sizeof(RECORD_HEAD) == 48, "queue file layout changed");
#if defined(__LP64__)
static_assert(sizeof(BOARD_INDEX_STRUCT) == 80, "board file layout changed");
static_assert(offsetof(BOARD_HEAD, mutex_rw) == 40, "board file layout changed");
#endif

// ---- loaded-file table (qbd_table.cpp) ----

// Copies the entry of a loaded file; sets g_lastError = ERROR_RECORD_NOT_EXIST when it isn't loaded
bool fetchtab(const char* dqname, TABLE_MSG& tabmsg);
bool IsLoaded(const char* dqname);
// Calls visit on each loaded file under the table lock, stopping when it returns false; returns that result
bool ForEachLoaded(const std::function<bool(const TABLE_MSG&)>& visit);

// Path of a QBD file in the QBD directory (SetQbdPath / GPLAT_QBD_PATH)
std::string QbdFilePath(const char* name);
// Creates the file, replacing an existing one, as fileSize zero bytes mapped for init to fill in
bool CreateQbdFile(const char* name, long fileSize, const std::function<void(char* base)>& init);
// Writes local time as "YYYY-MM-DD hh:mm:ss" into buf[20]
void FormatLocalTime(char* buf);

// hash1/hash2 hash the character sum modulo TABLESIZE, also for the board index: index slots are
// persisted in board files, so these must stay bit-exact.
inline int hash1(const char* s)
{
	int k = 0;
	for (; *s; ++s)
		k += static_cast<int>(*s);
	return k % TABLESIZE;
}

inline int hash2(const char* s)
{
	int k = 0;
	for (; *s; ++s)
		k += static_cast<int>(*s);
	const int remain = k % (TABLESIZE - 2);
	return remain == 0 ? 1 : remain;
}

// ---- board index (caller holds the index lock) ----

// Follows the probe chain to the slot holding name, or to the first never-used slot
inline int BoardProbe(const BOARD_INDEX_STRUCT* index, const char* name)
{
	int loc = hash1(name);
	const int c = hash2(name);
	while (index[loc].itemname[0] != '\0' && strcmp(index[loc].itemname, name) != 0)
		loc = (loc + c) % INDEXSIZE;
	return loc;
}

// Slot of a live item, or -1
inline int FindBoardItem(const BOARD_INDEX_STRUCT* index, const char* name)
{
	const int loc = BoardProbe(index, name);
	return (index[loc].itemname[0] == '\0' || index[loc].erased) ? -1 : loc;
}

inline std::mutex& StripeMutex(BOARD_HEAD* head, int loc)
{
	return head->mutex_rw_tag[loc & (MUTEXSIZE - 1)];
}

// Data area (item startpos is relative to it)
inline char* BoardData(BOARD_HEAD* head)
{
	return reinterpret_cast<char*>(head) + sizeof(BOARD_HEAD);
}

// Type area after the data area (item typeaddr is relative to it)
inline char* BoardTypeArea(BOARD_HEAD* head)
{
	return reinterpret_cast<char*>(head) + head->totalsize;
}

// ---- queue records ----

// RECORD_HEAD of a slot; the record data follows it. QueueRecord(head, head->num) is the type area.
inline char* QueueRecord(QUEUE_HEAD* head, int slot)
{
	return reinterpret_cast<char*>(head) + QUEUEHEADSIZE + slot * (head->size + RECORDHEADSIZE);
}

#pragma GCC visibility pop

#endif // !defined(QBD_H_INCLUDED_)
