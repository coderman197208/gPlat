# API 快速参考

## 连接管理

### connectgplat
```cpp
int connectgplat(const char* server, int port);
```
- **功能**: 连接到 gPlat 服务器
- **返回**: socket 文件描述符（失败返回 -1）
- **示例**: `int sockfd = connectgplat("127.0.0.1", 8777);`

### disconnectgplat
```cpp
void disconnectgplat(int sockfd);
```
- **功能**: 断开连接

---

## 队列操作 (Queue)

### CreateQ
```cpp
bool CreateQ(const char* lpFileName, int recordSize, int recordNum,
             int dateType, int operateMode, void* pType, int typeSize);
```
- **参数**:
  - `dateType`: DATATYPE_ASCII (0) 或 DATATYPE_BIN (1)
  - `operateMode`: NORMAL_MODE (0) 或 SHIFT_MODE (1)

### readq
```cpp
bool readq(int sockfd, const char* qname, void* record,
           int actsize, unsigned int* error);
```
- **功能**: 读取队列记录（FIFO）
- **错误码**: ERROR_QUEUE_NOT_EXIST, ERROR_QUEUE_EMPTY

### writeq
```cpp
bool writeq(int sockfd, const char* qname, void* record,
            int actsize, unsigned int* error);
```
- **功能**: 写入队列记录
- **错误码**: ERROR_QUEUE_NOT_EXIST, ERROR_QUEUE_FULL

### clearq
```cpp
bool clearq(int sockfd, const char* qname, unsigned int* error);
```
- **功能**: 清空队列

### readhead
```cpp
bool readhead(int sockfd, const char* qname, QUEUE_HEAD* head, unsigned int* error);
```
- **功能**: 读取队列头（记录大小、记录数、模式、读写指针、创建时间）
- **错误码**: ERROR_RECORD_NOT_EXIST（未载入）, ERROR_OPERATE_PROHIBIT（不是队列，如 BOARD）

### peekq
```cpp
bool peekq(int sockfd, const char* qname, int position, void* record,
           int actsize, RECORD_HEAD* recordhead, unsigned int* error);
```
- **功能**: 不移动读写指针地读取一条记录
- **参数**: `position` - PEEK_NEXT（readq 下一次将返回的记录）或 PEEK_LATEST（最近写入的记录），移位队列两者相同；`recordhead` 可为空，返回写入时间和来源 IP
- **错误码**: ERROR_DQ_EMPTY, ERROR_RECORDSIZE, ERROR_OPERATE_PROHIBIT

### listq
```cpp
bool listq(int sockfd, char* names, int buffsize, int* count, unsigned int* error);
```
- **功能**: 列出服务端已载入的队列，`names` 中依次存放 `count` 个以 `'\0'` 结尾的队列名（不含 BOARD）

### isemptyq / isfullq
```cpp
bool isemptyq(int sockfd, const char* qname, unsigned int* error);
bool isfullq(int sockfd, const char* qname, unsigned int* error);
```

---

## 公告板操作 (Board)

### CreateB
```cpp
bool CreateB(const char* lpFileName, int size);
```
- **参数**: `size` - 公告板总大小（字节）

### createtag
```cpp
bool createtag(int sockfd, const char* tagname, int tagsize,
               void* type, int typesize, unsigned int* error);
```
- **功能**: 创建标签

### listtags
```cpp
bool listtags(int sockfd, int start, char* buff, int buffsize,
              int* bytes, int* count, int* next, unsigned int* error);
```
- **功能**: 分页列出 BOARD 中的 tag 元数据。`buff` 中依次存放 `count` 条记录（共 `bytes` 字节，不对齐）：`TAG_META{itemsize, typesize}` + 以 `'\0'` 结尾的 tag 名 + `typesize` 字节的类型描述符
- **参数**: `start` - 首次为 0，之后传入上次返回的 `*next`；`*next == -1` 表示已全部列出；`buffsize` 建议为 MAXMSGLEN（16384）
- **说明**: 每页在服务端持锁读取，页与页之间不保证快照一致

### readb
```cpp
bool readb(int sockfd, const char* tagname, void* value, int actsize,
           unsigned int* error, timespec* timestamp);
```
- **功能**: 读取标签值
- **参数**: `timestamp` - 可选，返回数据时间戳

### writeb
```cpp
bool writeb(int sockfd, const char* tagname, void* value,
            int actsize, unsigned int* error);
```
- **功能**: 写入标签值

### readb_string / writeb_string
```cpp
bool readb_string(int sockfd, const char* tagname, char* value,
                  int buffersize, unsigned int* error, timespec* timestamp);
bool writeb_string(int sockfd, const char* tagname, const char* value,
                   unsigned int* error);
```
- **功能**: 字符串专用读写

---

## 数据库操作 (Database)

### createtable
```cpp
bool createtable(int sockfd, const char* tablename, int recordsize,
                 int maxcount, void* type, int typesize, unsigned int* error);
```

### inserttb
```cpp
bool inserttb(int sockfd, const char* tablename, void* record,
              int actsize, unsigned int* error);
```

### selecttb
```cpp
int selecttb(int sockfd, const char* tablename, int start, int count,
             void* records, int buffersize, unsigned int* error);
```
- **返回**: 实际读取的记录数

### cleartb
```cpp
bool cleartb(int sockfd, const char* tablename, unsigned int* error);
```

---

## 订阅机制 (Pub/Sub)

### subscribe
```cpp
bool subscribe(int sockfd, const char* tagname, unsigned int* error);
```
- **功能**: 订阅标签变化

### subscribedelaypost
```cpp
bool subscribedelaypost(int sockfd, const char* tagname,
                        const char* eventname, int delaytime,
                        unsigned int* error);
```
- **参数**: `delaytime` - 延迟时间（毫秒）

### waitpostdata
```cpp
bool waitpostdata(int sockfd, std::string& tagname,
                  int timeout, unsigned int* error);
```
- **功能**: 阻塞等待事件
- **参数**: `timeout` - 超时时间（毫秒）
- **返回**: `tagname` - 触发事件的标签名

### post
```cpp
bool post(int sockfd, const char* tagname, unsigned int* error);
```
- **功能**: 主动发布事件

---

## 请求-响应 (Request/Response)

### getresponse
```cpp
bool getresponse(int sockfd, const char* request_tag, void* request_value, int request_size,
                 const char* response_tag, void* response_value, int response_size,
                 unsigned int* error, int timeout_ms = 2000);
```
- **功能**: 写入 `request_tag` 并通知其订阅者（响应方），阻塞等待响应方 `writeb(response_tag)` 后把响应复制到 `response_value`
- **参数**: `request_size` / `response_size` 必须等于对应 tag 的大小（tag 需事先 `createtag`）；`timeout_ms` 必须 > 0，计时包含服务端排队时间
- **返回**: 超时返回 false，`error = ERROR_RESPONSE_TIMEOUT`；同名请求排队超过 64 个返回 `ERROR_REQUEST_QUEUE_FULL`
- **服务端语义**:
  - 同一 `request_tag` 同一时刻只处理 1 个请求，其余排队；一个 `response_tag` 只能对应一个 `request_tag`，否则返回 `ERROR_INVALID_PARAMETER`
  - 等待期间本连接其它订阅事件在服务端排队，`getresponse` 返回后再由 `waitpostdata` 取得
  - 出现过的 `response_tag` 被 `writeb` 时只写 Board、投递给等待的请求方，不通知普通订阅者；无人等待时丢弃并记日志（响应方 `writeb` 仍返回成功）。`writeb_notpost` / `writeb_string` 不触发投递
  - 请求方断开连接时释放其请求并继续处理排队中的下一个
- **限制**: 同一 `sockfd` 不支持多线程并发调用

---

## 错误码 (部分)

```cpp
#define ERROR_QUEUE_NOT_EXIST    1001
#define ERROR_QUEUE_EMPTY        1002
#define ERROR_QUEUE_FULL         1003
#define ERROR_BOARD_NOT_EXIST    2001
#define ERROR_TAG_NOT_EXIST      2002
#define ERROR_INVALID_PARAM      9001
#define ERROR_RESPONSE_TIMEOUT   1044  // getresponse 等待响应超时
#define ERROR_REQUEST_QUEUE_FULL 1045  // getresponse 同名请求排队已满
```

---

## 数据类型

```cpp
#define DATATYPE_ASCII  0
#define DATATYPE_BIN    1

#define NORMAL_MODE     0
#define SHIFT_MODE      1  // 满时覆盖最旧数据
```

---

## 常用模式

### 基本读写
```cpp
int sockfd = connectgplat("127.0.0.1", 8777);
unsigned int error = 0;

int value = 100;
writeb(sockfd, "temperature", &value, sizeof(value), &error);
readb(sockfd, "temperature", &value, sizeof(value), &error, NULL);

disconnectgplat(sockfd);
```

### 事件驱动
```cpp
subscribe(sockfd, "alarm", &error);
while (true) {
    std::string tagname;
    if (waitpostdata(sockfd, tagname, 5000, &error)) {
        printf("Event: %s\n", tagname.c_str());
        // 处理事件
    }
}
```

### 批量处理
```cpp
struct Record { int id; char data[64]; };
Record records[100];
int count = selecttb(sockfd, "history", 0, 100, records, sizeof(records), &error);
for (int i = 0; i < count; i++) {
    // 处理记录
}
```
