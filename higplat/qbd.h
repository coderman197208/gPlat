#if !defined(QBD_H_INCLUDED_)
#define QBD_H_INCLUDED_

#include <chrono>
#include <mutex>

#define MAXDQNAMELENTH 40	// 必须和higplat.h中的定义一致

#define ERROR_DQFILE_NOT_FOUND			1
#define ERROR_DQ_NOT_OPEN				2
#define ERROR_DQ_EMPTY					3
#define ERROR_DQ_FULL					4
#define ERROR_FILENAME_TOO_LONG			5
#define ERROR_FILE_IN_USE				6
#define ERROR_FILE_CREATE_FAILSURE		7
#define ERROR_FILE_OPEN_FAILSURE		8
#define ERROR_CREATE_FILEMAPPINGOBJECT	9
#define ERROR_OPEN_FILEMAPPINGOBJECT	10
#define ERROR_MAPVIEWOFFILE				11
#define ERROR_CREATE_MUTEX				12
#define ERROR_OPEN_MUTEX				13
#define ERROR_RECORDSIZE				14
#define ERROR_STARTPOSITION				15
#define ERROR_RECORD_ALREAD_EXIST		16
#define ERROR_TABLE_OVERFLOW			17
#define ERROR_RECORD_NOT_EXIST			18
#define ERROR_OPERATE_PROHIBIT			19
#define ERROR_ALREADY_OPEN				20
#define ERROR_ALREADY_CLOSE				21
#define ERROR_ALREADY_LOAD				22
#define ERROR_ALREADY_UNLOAD			23
#define ERROR_NO_SPACE			        24
#define ERROR_TABLE_NOT_EXIST			25
#define ERROR_TABLE_ALREADY_EXIST		26
#define ERROR_TABLE_ROWID				27
#define ERROR_ITEM_NOT_EXIST			28
#define ERROR_ITEM_ALREADY_EXIST		29
#define ERROR_ITEM_OVERFLOW				30
#define ERROR_SOCKET_NOT_CONNECTED      31
#define ERROR_MSGSIZE			        32
#define ERROR_BUFFER_SIZE		        33
#define ERROR_PARAMETER_SIZE	        34
#define CODE_QEMPTY						35
#define CODE_QFULL						36
#define STRING_TOO_LONG					37
#define BUFFER_TOO_SMALL				38
#define ERROR_INVALID_PARAMETER			39
#define ERROR_INVALID_RESPONSE			40
#define ERROR_BUFFER_TOO_SMALL			41

#define SHIFT_MODE		1
#define NORMAL_MODE		0
#define ASCII_TYPE		1
#define BINARY_TYPE		0
#define QUEUEHEADSIZE   sizeof(QUEUE_HEAD)
#define RECORDHEADSIZE  sizeof(RECORD_HEAD)

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
	pthread_mutex_t hMutex;
	std::mutex * pmutex_rw;
	bool erased;
	int count;
	long filesize;	// 文件大小 linux平台新增
};

struct QUEUE_HEAD
{
	int  qbdtype;
	int  dataType;			// 数据队列的类型，1为ASCII型；0为BINARY型
	int  operateMode;		// 1为移位队列，不判断溢出；0为通用队列
	int  num;				// 记录数
	int  size;				// 记录大小
	int  readPoint;			// 读指针
	int  writePoint;		// 写指针
	char createDate[20];	// 创建日期
	int  typesize;			// 类型序列化长度
	int  reserved;
};

struct RECORD_HEAD
{
	char createDate[20];
	char remoteIp[16];
	int  ack;				// 确认标志 0未确认1已确认
	int  index;				// 位置索引（0开始）
	int  reserve;			// 预留
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

struct DB_INDEX_STRUCT
{
	char  tablename[MAXDQNAMELENTH];
	int    startpos;		// reference to the beginning of data.
	int    recordsize;		// 记录大小
	int    maxcount;		// 最大记录数
	int    currcount;		// 当前记录数
	long   mutexaccess;     // 控制互斥访问的变量 //mark 未使用
	bool   erased;			// 表删除标志
	timespec timestamp;	// last write time
	int	   typeaddr;		// 类型起始地址 mark
	int	   typesize;		// 类型序列化长度
};

struct DB_HEAD
{
	int qbdtype;
	int counter;
	int totalsize;
	int typesize;		// 最后面的类型区的大小	mark
	int nextpos;		// reference to the beginning of unused data part.
	int nexttypepos;	// reference to the beginning of unused type part. mark
	int remain;
	int typeremain;		// 类型区剩余大小 mark
	int indexcount;
	std::mutex mutex_rw;
	std::mutex mutex_rw_tag[MUTEXSIZE];	//mark 尚未实现
	DB_INDEX_STRUCT index[INDEXSIZE];
};

struct BOARD_INFO
{
	int    totalsize;
	int    remainsize;
	int    tagcount_head;
	int    tagcount_act;
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