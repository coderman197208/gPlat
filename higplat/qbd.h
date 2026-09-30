#if !defined(QBD_H_INCLUDED_)
#define QBD_H_INCLUDED_

#include <chrono>
#include <mutex>

#include "../include/higplat.h"

#define MAXDQNAMELENTH 40
static_assert(MAXDQNAMELENTH == GPLAT_TAGNAME_SIZE, "tag name size mismatch");

#define QUEUEHEADSIZE   sizeof(QUEUE_HEAD)
#define RECORDHEADSIZE  sizeof(RECORD_HEAD)

enum class ErrorLevel {
    Deprecated = -1,
    Ignore = 0,
    Fatal = 1
};

struct ErrorInfo {
    ErrorLevel level;
    const char* message;
};

inline ErrorInfo GetErrorInfo(unsigned int errorCode) {
    switch (errorCode) {
        case 0:
            return { ErrorLevel::Ignore, "no error" };
        case ERROR_DQFILE_NOT_FOUND:
            return { ErrorLevel::Deprecated, "" };
        case ERROR_DQ_NOT_OPEN:
            return { ErrorLevel::Deprecated, "" };
        case ERROR_DQ_EMPTY:
            return { ErrorLevel::Ignore, "queue empty" };
        case ERROR_DQ_FULL:
            return { ErrorLevel::Ignore, "queue full" };
        case ERROR_FILENAME_TOO_LONG:
            return { ErrorLevel::Ignore, "filename too long" };
        case ERROR_FILE_IN_USE:
            return { ErrorLevel::Ignore, "file already in use" };
        case ERROR_FILE_CREATE_FAILSURE:
            return { ErrorLevel::Ignore, "failed to create file" };
        case ERROR_FILE_OPEN_FAILSURE:
            return { ErrorLevel::Deprecated, "" };
        case ERROR_CREATE_FILEMAPPINGOBJECT:
            return { ErrorLevel::Deprecated, "" };
        case ERROR_OPEN_FILEMAPPINGOBJECT:
            return { ErrorLevel::Deprecated, "" };
        case ERROR_MAPVIEWOFFILE:
            return { ErrorLevel::Deprecated, "" };
        case ERROR_CREATE_MUTEX:
            return { ErrorLevel::Deprecated, "" };
        case ERROR_OPEN_MUTEX:
            return { ErrorLevel::Deprecated, "" };
        case ERROR_RECORDSIZE:
            return { ErrorLevel::Fatal, "record size invalid" };
        case ERROR_STARTPOSITION:
            return { ErrorLevel::Ignore, "bad start position" };
        case ERROR_RECORD_ALREAD_EXIST:
            return { ErrorLevel::Deprecated, "" };
        case ERROR_TABLE_OVERFLOW:
            return { ErrorLevel::Ignore, "table overflow" };
        case ERROR_RECORD_NOT_EXIST:
            return { ErrorLevel::Ignore, "record not exist" };
        case ERROR_OPERATE_PROHIBIT:
            return { ErrorLevel::Ignore, "unsupported operation" };
        case ERROR_ALREADY_OPEN:
            return { ErrorLevel::Deprecated, "" };
        case ERROR_ALREADY_CLOSE:
            return { ErrorLevel::Deprecated, "" };
        case ERROR_ALREADY_LOAD:
            return { ErrorLevel::Ignore, "queue already loaded" };
        case ERROR_ALREADY_UNLOAD:
            return { ErrorLevel::Deprecated, "" };
        case ERROR_NO_SPACE:
            return { ErrorLevel::Ignore, "no space" };
        case ERROR_TABLE_NOT_EXIST:
            return { ErrorLevel::Ignore, "table not exist" };
        case ERROR_TABLE_ALREADY_EXIST:
            return { ErrorLevel::Ignore, "table already exist" };
        case ERROR_TABLE_ROWID:
            return { ErrorLevel::Ignore, "table bad row id" };
        case ERROR_ITEM_NOT_EXIST:
            return { ErrorLevel::Ignore, "item not exist" };
        case ERROR_ITEM_ALREADY_EXIST:
            return { ErrorLevel::Ignore, "item already exist" };
        case ERROR_ITEM_OVERFLOW:
            return { ErrorLevel::Ignore, "item overflow" };
        case ERROR_SOCKET_NOT_CONNECTED:
            return { ErrorLevel::Deprecated, "" };
        case ERROR_MSGSIZE:
            return { ErrorLevel::Deprecated, "" };
        case ERROR_BUFFER_SIZE:
            return { ErrorLevel::Deprecated, "" };
        case ERROR_PARAMETER_SIZE:
            return { ErrorLevel::Ignore, "parameter size invalid" };
        case CODE_QEMPTY:
            return { ErrorLevel::Deprecated, "" };
        case CODE_QFULL:
            return { ErrorLevel::Deprecated, "" };
        case STRING_TOO_LONG:
            return { ErrorLevel::Ignore, "string too long" };
        case BUFFER_TOO_SMALL:
            return { ErrorLevel::Ignore, "buffer too small" };
        case ERROR_INVALID_PARAMETER:
            return { ErrorLevel::Ignore, "invalid parameter" };
        case ERROR_INVALID_RESPONSE:
            return { ErrorLevel::Ignore, "invalid response" };
        case ERROR_BUFFER_TOO_SMALL:
            return { ErrorLevel::Fatal, "buffer too small" };
        case ERROR_TAG_NOT_EXIST:
			return { ErrorLevel::Fatal, "tag not exist" };
        case ERROR_WAIT_TIMEOUT:
			return { ErrorLevel::Ignore, "wait post timeout" };
        case ERROR_RESPONSE_TIMEOUT:
			return { ErrorLevel::Ignore, "wait response timeout" };
        case ERROR_REQUEST_QUEUE_FULL:
			return { ErrorLevel::Ignore, "request queue full" };
        default:
            return { ErrorLevel::Ignore, "unknown error" };
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