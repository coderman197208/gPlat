#if !defined(HIGPLAT_H_INCLUDED_)
#define HIGPLAT_H_INCLUDED_

#include <stdbool.h>
#include <time.h>

#define MY_ERR_OFFSET    1000
#define ERROR_DQFILE_NOT_FOUND			(MY_ERR_OFFSET + 1)
#define ERROR_DQ_NOT_OPEN				(MY_ERR_OFFSET + 2)
#define ERROR_DQ_EMPTY					(MY_ERR_OFFSET + 3)
#define ERROR_DQ_FULL					(MY_ERR_OFFSET + 4)
#define ERROR_FILENAME_TOO_LONG			(MY_ERR_OFFSET + 5)
#define ERROR_FILE_IN_USE				(MY_ERR_OFFSET + 6)
#define ERROR_FILE_CREATE_FAILSURE		(MY_ERR_OFFSET + 7)
#define ERROR_FILE_OPEN_FAILSURE		(MY_ERR_OFFSET + 8)
#define ERROR_CREATE_FILEMAPPINGOBJECT	(MY_ERR_OFFSET + 9)
#define ERROR_OPEN_FILEMAPPINGOBJECT	(MY_ERR_OFFSET + 10)
#define ERROR_MAPVIEWOFFILE				(MY_ERR_OFFSET + 11)
#define ERROR_CREATE_MUTEX				(MY_ERR_OFFSET + 12)
#define ERROR_OPEN_MUTEX				(MY_ERR_OFFSET + 13)
#define ERROR_RECORDSIZE				(MY_ERR_OFFSET + 14)
#define ERROR_STARTPOSITION				(MY_ERR_OFFSET + 15)
#define ERROR_RECORD_ALREAD_EXIST		(MY_ERR_OFFSET + 16)
#define ERROR_TABLE_OVERFLOW			(MY_ERR_OFFSET + 17)
#define ERROR_RECORD_NOT_EXIST			(MY_ERR_OFFSET + 18)
#define ERROR_OPERATE_PROHIBIT			(MY_ERR_OFFSET + 19)
#define ERROR_ALREADY_OPEN				(MY_ERR_OFFSET + 20)
#define ERROR_ALREADY_CLOSE				(MY_ERR_OFFSET + 21)
#define ERROR_ALREADY_LOAD				(MY_ERR_OFFSET + 22)
#define ERROR_ALREADY_UNLOAD			(MY_ERR_OFFSET + 23)
#define ERROR_NO_SPACE			        (MY_ERR_OFFSET + 24)
#define ERROR_TABLE_NOT_EXIST			(MY_ERR_OFFSET + 25)
#define ERROR_TABLE_ALREADY_EXIST		(MY_ERR_OFFSET + 26)
#define ERROR_TABLE_ROWID				(MY_ERR_OFFSET + 27)
#define ERROR_ITEM_NOT_EXIST			(MY_ERR_OFFSET + 28)
#define ERROR_ITEM_ALREADY_EXIST		(MY_ERR_OFFSET + 29)
#define ERROR_ITEM_OVERFLOW				(MY_ERR_OFFSET + 30)
#define ERROR_SOCKET_NOT_CONNECTED      (MY_ERR_OFFSET + 31)
#define ERROR_MSGSIZE			        (MY_ERR_OFFSET + 32)
#define ERROR_BUFFER_SIZE		        (MY_ERR_OFFSET + 33)
#define ERROR_PARAMETER_SIZE	        (MY_ERR_OFFSET + 34)
#define CODE_QEMPTY						(MY_ERR_OFFSET + 35)
#define CODE_QFULL						(MY_ERR_OFFSET + 36)
#define STRING_TOO_LONG			        (MY_ERR_OFFSET + 37)
#define BUFFER_TOO_SMALL			    (MY_ERR_OFFSET + 38)
#define ERROR_INVALID_PARAMETER			(MY_ERR_OFFSET + 39)
#define ERROR_INVALID_RESPONSE			(MY_ERR_OFFSET + 40)
#define ERROR_BUFFER_TOO_SMALL			(MY_ERR_OFFSET + 41)
#define ERROR_TAG_NOT_EXIST 			(MY_ERR_OFFSET + 42)
#define ERROR_WAIT_TIMEOUT              (MY_ERR_OFFSET + 43)
#define ERROR_RESPONSE_TIMEOUT          (MY_ERR_OFFSET + 44)
#define ERROR_REQUEST_QUEUE_FULL        (MY_ERR_OFFSET + 45)

#pragma pack( push, enter_qbdtype_h_, 8)

typedef struct QUEUE_HEAD
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
} QUEUE_HEAD;

typedef struct RECORD_HEAD
{
	char createDate[20];
	char remoteIp[16];
	int  ack;				// 确认标志 0未确认1已确认
	int  index;				// 位置索引（0开始）
	int  reserve;			// 预留
} RECORD_HEAD;

typedef struct BOARD_INFO
{
	int    totalsize;
	int    remainsize;
	int    tagcount_head;
	int    tagcount_act;
} BOARD_INFO;

// listtags/ListTags 输出的每条记录：TAG_META + 以 '\0' 结尾的 tag 名 + typesize 字节的类型描述符（不对齐）
typedef struct TAG_META
{
	int    itemsize;
	int    typesize;
} TAG_META;

#pragma pack( pop, enter_qbdtype_h_ )

#define SHIFT_MODE		1
#define NORMAL_MODE		0
#define ASCII_TYPE		1
#define BINARY_TYPE		0

// peekq 的读取位置
#define PEEK_NEXT		0	// readq 下一次将返回的记录
#define PEEK_LATEST		1	// 最近一次写入的记录

#define GPLAT_MAX_DATA_SIZE	16384	// 单次读写数据的最大长度
#define GPLAT_TAGNAME_SIZE	40		// tag 名缓冲区长度（含 '\0'）

#ifdef __cplusplus
extern "C" {
#endif

struct timespec;

// ---- Network API ----
int  connectgplat(const char* server, int port);
void disconnectgplat(int sockfd);
bool readq(int sockfd, const char* qname, void* record, int actsize, unsigned int* error);
bool writeq(int sockfd, const char* qname, void* record, int actsize, unsigned int* error);
bool clearq(int sockfd, const char* qname, unsigned int* error);
bool readhead(int sockfd, const char* qname, QUEUE_HEAD* head, unsigned int* error);
bool peekq(int sockfd, const char* qname, int position, void* record, int actsize, RECORD_HEAD* recordhead, unsigned int* error);
bool listq(int sockfd, char* names, int buffsize, int* count, unsigned int* error);
bool listtags(int sockfd, int start, char* buff, int buffsize, int* bytes, int* count, int* next, unsigned int* error);
bool readb(int sockfd, const char* tagname, void* value, int actsize, unsigned int* error, struct timespec* timestamp);
bool writeb(int sockfd, const char* tagname, void* value, int actsize, unsigned int* error);
bool writeb_notpost(int sockfd, const char* tagname, void* value, int actsize, unsigned int* error);
bool subscribe(int sockfd, const char* tagname, unsigned int* error);
bool subscribedelaypost(int sockfd, const char* tagname, const char* eventname, int delaytime, unsigned int* error);
bool createtag(int sockfd, const char* tagname, int tagsize, void* type, int typesize, unsigned int* error);
bool deletetag(int sockfd, const char* tagname, unsigned int* error);
// tagnamesize >= GPLAT_TAGNAME_SIZE; on timeout returns true with error ERROR_WAIT_TIMEOUT and tagname "WAIT_TIMEOUT"
bool waitpostdata(int sockfd, char* tagname, int tagnamesize, void* value, int buffersize, int timeout, unsigned int* error);
bool readb_string(int sockfd, const char* tagname, char* value, int buffersize, unsigned int* error, struct timespec* timestamp);
bool writeb_string(int sockfd, const char* tagname, const char* value, unsigned int* error);
bool writeb_string_notpost(int sockfd, const char* tagname, const char* value, unsigned int* error);
bool readtype(int sockfd, const char* qbdname, const char* tagname, void* inbuff, int buffsize, int* ptypesize, unsigned int* error);
bool clearb(int sockfd, unsigned int* error);
bool readboardinfo(int sockfd, void* info, int infosize, unsigned int* error);
bool createqueue(int sockfd, const char* queuename, int recordsize, int recordnum, int operatemode, void* type, int typesize, unsigned int* error);
bool getresponse(int sockfd, const char* request_tag, void* request_value, int request_size, const char* response_tag, void* response_value, int response_size, unsigned int* error, int timeout_ms);

bool write_plc_string(int sockfd, const char* tagname, const char* str, unsigned int* error);
bool write_plc_bool(int sockfd, const char* tagname, bool value, unsigned int* error);
bool write_plc_short(int sockfd, const char* tagname, short value, unsigned int* error);
bool write_plc_ushort(int sockfd, const char* tagname, unsigned short value, unsigned int* error);
bool write_plc_int(int sockfd, const char* tagname, int value, unsigned int* error);
bool write_plc_uint(int sockfd, const char* tagname, unsigned int value, unsigned int* error);
bool write_plc_float(int sockfd, const char* tagname, float value, unsigned int* error);
bool registertag(int sockfd, const char* tagname, unsigned int* error);

// message may be NULL
bool IsFatalError(unsigned int error, const char** message);

// ---- Local API ----
bool CreateB(const char* lpFileName, int size);
bool CreateItem(const char* lpBoardName, const char* lpItemName, int itemSize, void* pType, int typeSize);
bool DeleteItem(const char* lpBoardName, const char* lpItemName);
bool CreateQ(const char* lpFileName, int recordSize, int recordNum, int dateType, int operateMode, void* pType, int typeSize);
bool CreateAndLoadQ(const char* lpFileName, int recordSize, int recordNum, int dataType, int operateMode, void* pType, int typeSize);
bool LoadQ(const char* lpDqName);
void SetQbdPath(const char* path);
bool ReadQ(const char* lpDqName, void* lpRecord, int actSize, char* remoteIp);
bool WriteQ(const char* lpDqName, void* lpRecord, int actSize, const char* remoteIp);
bool ClearQ(const char* lpDqName);
bool ReadHead(const char* lpDqName, void* lpHead);
bool PeekQRecord(const char* lpDqName, int position, void* lpRecord, int actSize, RECORD_HEAD* lpRecordHead);
bool ListQ(char* names, int buffSize, int* namesSize, int* count);
bool ReadB(const char* lpBoardName, const char* lpItemName, void* lpItem, int actSize, struct timespec* timestamp);
bool ReadB_String(const char* lpBulletinName, const char* lpItemName, void* lpItem, int actSize, struct timespec* timestamp);
bool ReadB_String2(const char* lpBulletinName, const char* lpItemName, void* lpItem, int actSize, int* strLength, struct timespec* timestamp);
bool WriteB(const char* lpBulletinName, const char* lpItemName, void* lpItem, int actSize, void* lpSubItem, int actSubSize);
bool WriteB_String(const char* lpBulletinName, const char* lpItemName, void* lpItem, int actSize, void* lpSubItem, int actSubSize);
bool ClearB(const char* lpBoardName);
unsigned int GetLastErrorQ(void);
bool ReadType(const char* lpDqName, const char* lpItemName, void* inBuff, int buffSize, int* pTypeSize);
bool ReadBoardInfo(const char* lpBoardName, BOARD_INFO* boardinfo);
bool ListTags(const char* lpBoardName, int start, char* buff, int buffSize, int* bytes, int* count, int* next);

#ifdef __cplusplus
}
#endif

#endif // HIGPLAT_H_INCLUDED_