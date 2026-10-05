#if !defined(QBD_H_INCLUDED_)
#define QBD_H_INCLUDED_

#include <chrono>
#include <cstring>
#include <mutex>

#include "../include/higplat.h"

#define MAXDQNAMELENTH 40
static_assert(MAXDQNAMELENTH == GPLAT_TAGNAME_SIZE, "tag name size mismatch");

#define QUEUEHEADSIZE   sizeof(QUEUE_HEAD)
#define RECORDHEADSIZE  sizeof(RECORD_HEAD)

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
        return { ErrorCategory::Connection, ErrnoMessage((int)errorCode) };

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

enum{
	QUEUE_T,
	BOARD_T,
	DATABASE_T
};
#define MUTEXSIZE	  64	// 一个BOARD或DB中读写锁的数量，必须是2的n次幂 mark，必须和dataqueue.h中的定义一致
#define TABLESIZE     277	// 必须为质数
#define INDEXSIZE     7177 	// 必须为质数，必须和higplat.h中的定义一致
#define TYPEMAXSIZE   2048  // 数据类型的最大序列化长度   必须与msg.h中的MAXMSGLEN一致	//mark 与QbdServer项目中MyIOCP::HandleSUBSCRIBE里的缓冲区大小有矛盾，似乎没必要那么大
#define TYPEAVGSIZE	  32	// 数据类型的平均序列化长度	mark

#pragma pack( push, enter_qbd_h_, 8)

struct TABLE_MSG
{
	char dqname[MAXDQNAMELENTH];
	int  hFile;
	void* lpMapAddress;
	int hMapFile;
	pthread_mutex_t* hMutex;	// 队列锁，fetchtab 返回的副本必须指向同一把锁
	std::mutex * pmutex_rw;
	bool erased;
	int count;
	long filesize;	// 文件大小 linux平台新增
};

//clock_gettime(CLOCK_REALTIME, &ts);
//printf("秒: %ld, 纳秒: %ld\n", ts.tv_sec, ts.tv_nsec);
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
	int typesize;		// 最后面的类型区的大小	mark
	int nextpos;		// reference to the beginning of unused date part.
	int nexttypepos;	// reference to the beginning of unused type part. mark
	int remain;
	int typeremain;		// 类型区剩余大小 mark
	int indexcount;
	std::mutex mutex_rw;
	std::mutex mutex_rw_tag[MUTEXSIZE];
	BOARD_INDEX_STRUCT index[INDEXSIZE];
};

bool inserttab(const struct TABLE_MSG &tabmsg);
bool fetchtab(const char* dqname, struct TABLE_MSG &tabmsg);
bool fetchtab1(const char* dqname, struct TABLE_MSG &tabmsg);
bool deletetab(const char* dqname, struct TABLE_MSG &tabmsg);
inline int  hash1(const char* s);
inline int  hash2(const char* s);
inline void gettime(const char* timebuf);

#pragma pack( pop, enter_qbd_h_ )

#endif // !defined(QBD_H_INCLUDED_)