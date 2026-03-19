#if !defined(HIGPLAT_H_INCLUDED_)
#define HIGPLAT_H_INCLUDED_

#include <cstring>
#include <string>
#include <type_traits>

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

#pragma pack( push, enter_qbdtype_h_, 8)

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

struct BOARD_INFO
{
	int    totalsize;
	int    remainsize;
	int    tagcount_head;
	int    tagcount_act;
};

#pragma pack( pop, enter_qbdtype_h_ )

#define SHIFT_MODE		1
#define NORMAL_MODE		0
#define ASCII_TYPE		1
#define BINARY_TYPE		0

extern "C" int  connectgplat(const char* server, int port);
extern "C" void disconnectgplat(int sockfd);
extern "C" bool readq(int sockfd, const char* qname, void* record, int actsize, unsigned int* error);
extern "C" bool writeq(int sockfd, const char* qname, void* record, int actsize, unsigned int* error);
extern "C" bool clearq(int sockfd, const char* qname, unsigned int* error);
extern "C" bool readb(int sockfd, const char* tagname, void* value, int actsize, unsigned int* error, timespec* timestamp = 0);
extern "C" bool writeb(int sockfd, const char* tagname, void* value, int actsize, unsigned int* error);
extern "C" bool writeb_notpost(int sockfd, const char* tagname, void* value, int actsize, unsigned int* error);
extern "C" bool subscribe(int sockfd, const char* tagname, unsigned int* error);
extern "C" bool subscribedelaypost(int sockfd, const char* tagname, const char* eventname, int delaytime, unsigned int* error);
extern "C" bool createtag(int sockfd, const char* tagname, int tagsize, void* type, int typesize, unsigned int* error);
extern "C" bool deletetag(int sockfd, const char* tagname, unsigned int* error);
extern "C" bool waitpostdata(int sockfd, std::string& tagname, void* value, int buffersize, int timeout, unsigned int* error);
extern "C" bool readb_string(int sockfd, const char* tagname, char* value, int buffersize, unsigned int* error, timespec*timestamp=0);
extern "C" bool writeb_string(int sockfd, const char* tagname, const char* value, unsigned int* error);
extern "C" bool readb_string2(int sockfd, const char* tagname, std::string& value, unsigned int* error, timespec* timestamp = 0);
extern "C" bool writeb_string2(int sockfd, const char* tagname, std::string value, unsigned int* error);
extern "C" bool readtype(int sockfd, const char* qbdname, const char* tagname, void* inbuff, int buffsize, int* ptypesize, unsigned int* error);
extern "C" bool clearb(int sockfd, unsigned int* error);
extern "C" bool readboardinfo(int sockfd, const void* info, int infosize, unsigned int* error);
extern "C" bool createqueue(int sockfd, const char* queuename, int recordsize, int recordnum, int operatemode, void* type, int typesize, unsigned int* error);

extern "C" bool write_plc_string(int sockfd, const char* tagname, std::string str, unsigned int* error);
extern "C" bool write_plc_bool(int sockfd, const char* tagname, bool value, unsigned int* error);
template<typename T> bool write_plc_bool(int sockfd, const char* tagname, T value, unsigned int* error) = delete;
extern "C" bool write_plc_short(int sockfd, const char* tagname, short value, unsigned int* error);
template<typename T> bool write_plc_short(int sockfd, const char* tagname, T value, unsigned int* error) = delete;
extern "C" bool write_plc_ushort(int sockfd, const char* tagname, unsigned short value, unsigned int* error);
template<typename T> bool write_plc_ushort(int sockfd, const char* tagname, T value, unsigned int* error) = delete;
extern "C" bool write_plc_int(int sockfd, const char* tagname, int value, unsigned int* error);
template<typename T> bool write_plc_int(int sockfd, const char* tagname, T value, unsigned int* error) = delete;
extern "C" bool write_plc_uint(int sockfd, const char* tagname, unsigned int value, unsigned int* error);
template<typename T> bool write_plc_uint(int sockfd, const char* tagname, T value, unsigned int* error) = delete;
extern "C" bool write_plc_float(int sockfd, const char* tagname, float value, unsigned int* error);
template<typename T> bool write_plc_float(int sockfd, const char* tagname, T value, unsigned int* error) = delete;
extern "C" bool registertag(int sockfd, const char* tagname, unsigned int* error);

extern "C" bool CreateB(const char* lpFileName, int size);
extern "C" bool CreateItem(const char* lpBoardName, const char* lpItemName, int itemSize, void* pType = 0, int typeSize = 0);
extern "C" bool DeleteItem(const char* lpBoardName, const char* lpItemName);
extern "C" bool CreateQ(const char* lpFileName, int recordSize, int recordNum, int dateType, int operateMode, void* pType = 0, int typeSize = 0);
extern "C" bool LoadQ(const char* lpDqName );
extern "C" void SetQbdPath(const char* path);
extern "C" bool ReadQ(const char* lpDqName, void  *lpRecord, int actSize, char* remoteIp=0 );
extern "C" bool WriteQ(const char* lpDqName, void  *lpRecord, int actSize=0, const char* remoteIp=0 );
extern "C" bool ClearQ(const char* lpDqName );
extern "C" bool ReadB(const char* lpBoardName, const char* lpItemName, void* lpItem, int actSize, timespec* timestamp = 0);
extern "C" bool ReadB_String(const char* lpBulletinName, const char* lpItemName, void*lpItem, int actSize, timespec*timestamp=0);
extern "C" bool ReadB_String2(const char* lpBulletinName, const char* lpItemName, void* lpItem, int actSize, int& strLength, timespec* timestamp);
extern "C" bool WriteB(const char* lpBulletinName, const char* lpItemName, void* lpItem, int actSize, void* lpSubItem = 0, int actSubSize = 0);
extern "C" bool WriteB_String(const char* lpBulletinName, const char* lpItemName, void *lpItem, int actSize, void *lpSubItem = 0, int actSubSize = 0);
extern "C" bool ClearB(const char* lpBoardName);
extern "C" bool GetLastErrorQ();
extern "C" bool ReadType(const char* lpDqName, const char* lpItemName, void* inBuff, int buffSize, int* pTypeSize);
extern "C" bool ReadBoardInfo(const char* lpBoardName, BOARD_INFO* boardinfo);

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

//更稳一点的字符串版本
//如果字符串可能不是完整填满，最好再带长度：
/*
template<typename T>
T read_value(const char* buffer, size_t size) {
	if constexpr (std::is_same_v<T, std::string>) {
		return std::string(buffer, strnlen(buffer, size));
	}
	else if constexpr (std::is_same_v<T, const char*>) {
		return buffer;
	}
	else {
		static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
		if (size < sizeof(T)) {
			throw std::out_of_range("buffer too small");
		}
		T result{};
		std::memcpy(&result, buffer, sizeof(T));
		return result;
	}
}
*/

#endif // HIGPLAT_H_INCLUDED_