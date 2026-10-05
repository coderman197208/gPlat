# API 快速参考

`include/higplat.h` 是 `libhigplat.so` 的纯 C 头文件（C99 起可用，C++ 中自动 `extern "C"`）：只用 C 类型、无默认参数、不抛异常（错误只通过返回值和 `error` 返回，`error` 为 NULL 时直接返回 false）。C++ 调用方建议使用 `include/gplat_connection.h`（见下文 GplatConnection）。

- `GPLAT_MAX_DATA_SIZE`（16384）：单次读写数据的最大长度
- `GPLAT_TAGNAME_SIZE`（40）：tag 名缓冲区长度（含 `'\0'`）
- `int GetErrorCategory(unsigned int error, const char** message)`：错误码的类别 `GPLAT_ERRCAT_*`，`message` 可为 NULL（errno 的描述在本线程下次调用前有效）。分类表见 `Doc/ERROR_CODE.md`
  - `GPLAT_ERRCAT_RESULT`（0）：运行结果，调用方按返回码分支，如 `ERROR_TAG_NOT_EXIST`、`ERROR_DQ_EMPTY`、`ERROR_WAIT_TIMEOUT`、`ERROR_RESPONSE_TIMEOUT`
  - `GPLAT_ERRCAT_USAGE`（1）：编程错误，如 `ERROR_INVALID_PARAMETER`、`ERROR_RECORDSIZE`、`STRING_TOO_LONG`、`ERROR_BUFFER_TOO_SMALL`
  - `GPLAT_ERRCAT_CONNECTION`（2）：连接不可用：errno（< `MY_ERR_OFFSET`，send/recv 失败）、`ERROR_SOCKET_NOT_CONNECTED`、`ERROR_INVALID_RESPONSE`
- `bool IsFatalError(unsigned int error, const char** message)`：兼容旧接口，等价于 `GetErrorCategory(error, message) == GPLAT_ERRCAT_USAGE`
- `void SetErrorHook(GPLAT_ERROR_HOOK hook, void* user)`：设置错误钩子 `void (*)(unsigned int error, int category, const char* message, const char* func, void* user)`。带 `unsigned int* error` 的网络 API 返回时若错误码非 0（含 `waitpostdata` 超时），就调用一次钩子；`connectgplat` 失败时也调用一次（`error` 为 errno 或 `ERROR_INVALID_PARAMETER`，`message` 附带 host:port）；`func` 为 API 函数名，`message` 和 `func` 只在回调期间有效。默认钩子把 USAGE 和 CONNECTION 类错误整行写到 stderr（如 `[higplat usage] writeb: record size invalid (code 1014)`），`hook` 传 NULL 关闭输出。线程安全；钩子可能在多个线程上并发执行，不得抛异常
- **fd 所有权**：`sockfd` 始终归调用方，网络 API 失败时从不 `close` 它。返回 CONNECTION 类错误时库已 `shutdown` 该连接（服务端随即断开并清理订阅），之后该 fd 上的调用都以 CONNECTION 类错误失败，调用方应 `disconnectgplat` 后重连并重新订阅；RESULT 和 USAGE 类错误不影响连接。`send` 使用 `MSG_NOSIGNAL`，对端断开不会以 SIGPIPE 终止进程

```c
static void my_hook(unsigned int error, int category, const char* message, const char* func, void* user)
{
    if (category != GPLAT_ERRCAT_RESULT)
        my_log("%s: %s (code %u)", func, message, error);
}
SetErrorHook(my_hook, NULL);    // 进程启动时设置一次
```

## 连接管理

### connectgplat
```cpp
int connectgplat(const char* server, int port);
```
- **功能**: 连接到 gPlat 服务器
- **返回**: socket 文件描述符；失败返回 -1 并设置 errno（2 秒超时为 `ETIMEDOUT`），同时经错误钩子上报一次
- **示例**: `int sockfd = connectgplat("127.0.0.1", 8777);`

### disconnectgplat
```cpp
void disconnectgplat(int sockfd);
```
- **功能**: 断开连接；任何网络 API 失败后（包括 CONNECTION 类错误）都由调用方用它关闭 fd

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
- **说明**: `typesize` 必须在 1~100 之间；`typesize == 0`（或 > 100）返回 `false` 且 `*error = ERROR_PARAMETER_SIZE`

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
- **参数**: `timestamp` - 可为 NULL，非空时返回数据时间戳

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
- **错误**: tag 不存在返回 `ERROR_TAG_NOT_EXIST`，连接和已有订阅保留

### subscribedelaypost
```cpp
bool subscribedelaypost(int sockfd, const char* tagname,
                        const char* eventname, int delaytime,
                        unsigned int* error);
```
- **参数**: `delaytime` - 延迟时间（毫秒）

### waitpostdata
```cpp
bool waitpostdata(int sockfd, char* tagname, int tagnamesize,
                  void* value, int buffersize, int timeout, unsigned int* error);
```
- **功能**: 阻塞等待事件
- **参数**: `tagname` - 返回触发事件的标签名，`tagnamesize` 必须 >= `GPLAT_TAGNAME_SIZE`（否则 `ERROR_INVALID_PARAMETER`）；`timeout` - 超时时间（毫秒，-1 永久等待）
- **超时**: 返回 true，`error = ERROR_WAIT_TIMEOUT`，`tagname` 为 `"WAIT_TIMEOUT"`
- **缓冲区不足**: 事件数据比 `buffersize` 大时返回 false，`error = ERROR_BUFFER_TOO_SMALL`；该事件被丢弃，连接和订阅保留

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
                 unsigned int* error, int timeout_ms);
```
- **功能**: 写入 `request_tag` 并通知其订阅者（响应方），阻塞等待响应方 `writeb(response_tag)` 后把响应复制到 `response_value`
- **参数**: `request_size` / `response_size` 必须等于对应 tag 的大小（tag 需事先 `createtag`）；`timeout_ms` 必须 > 0（建议 2000，GplatConnection 默认值），计时包含服务端排队时间
- **返回**: 超时返回 false，`error = ERROR_RESPONSE_TIMEOUT`；同名请求排队超过 64 个返回 `ERROR_REQUEST_QUEUE_FULL`
- **服务端语义**:
  - 同一 `request_tag` 同一时刻只处理 1 个请求，其余排队；一个 `response_tag` 只能对应一个 `request_tag`，否则返回 `ERROR_INVALID_PARAMETER`；该绑定在 `response_tag`（或其 `request_tag`，且无挂起请求时）经 `deletetag` 删除后解除
  - 等待期间本连接其它订阅事件在服务端排队，`getresponse` 返回后再由 `waitpostdata` 取得
  - 出现过的 `response_tag` 被 `writeb` 时只写 Board、投递给等待的请求方，不通知普通订阅者；无人等待时丢弃并记日志（响应方 `writeb` 仍返回成功）。`writeb_notpost` / `writeb_string` 不触发投递
  - 请求方断开连接时释放其请求并继续处理排队中的下一个
- **限制**: 同一 `sockfd` 不支持多线程并发调用

---

## C++ 封装类 GplatConnection

头文件 `include/gplat_connection.h`，是 `higplat.h` 的 C++ 封装，header-only（全部 inline 转调上面的 C 接口）。`std::string`、异常、默认参数等 C++ 特性只存在于调用方的编译单元，`libhigplat.so` 边界上只有 C ABI；类布局变化只需用户重新编译。另提供 `read_value<T>(char*)` 模板，用于从 `waitpostdata` 缓冲区取值。

```cpp
GplatConnection conn("127.0.0.1", 8777);   // 构造只保存地址，不连接
if (!conn.open()) { /* 连接失败 */ }        // 已打开时直接返回 true
int value = 100;
unsigned int err = conn.writeb("temperature", &value, sizeof(value));   // 0 表示成功
std::string s;
err = conn.readb_string("name", s);
conn.close();                              // 析构时也会自动关闭
```

- **与 C 接口的差异**: 去掉 `sockfd` 参数；去掉 `unsigned int* error`，改为返回值（`[[nodiscard]] unsigned int`，0 为成功，非 0 只会是 `GPLAT_ERRCAT_RESULT` 类错误码）；名称参数为 `const std::string&`，写入缓冲区为 `const void*`
- **重载**: `readb_string(tag, char*, int, timespec* = nullptr)` / `readb_string(tag, std::string&, timespec* = nullptr)`（内部以 `GPLAT_MAX_DATA_SIZE` 缓冲区调用 C 版，失败时不修改 `value`）；`writeb_string` / `writeb_string_notpost` 均有 `const char*` 与 `const std::string&` 两个版本；`waitpostdata(std::string& tagname, ...)`；`write_plc_*` 有 `=delete` 模板，禁止隐式类型转换
- **已封装**: `readq` `writeq` `clearq` `peekq` `readb` `writeb` `writeb_notpost` `readb_string` `writeb_string` `writeb_string_notpost` `subscribe` `subscribedelaypost` `waitpostdata` `getresponse` `write_plc_{string,bool,short,ushort,int,uint,float}`
- **waitpostdata 超时**: 返回 `ERROR_WAIT_TIMEOUT`，`tagname` 置为 `"WAIT_TIMEOUT"`
- **异常**: 按 `GetErrorCategory` 的类别决定，所有异常都派生自 `GplatError : std::runtime_error`（`code()` 为错误码，`what()` 为 `"<描述> (Code: N)"`），抛出前已更新连接状态：
  - RESULT（如 `ERROR_TAG_NOT_EXIST`、`ERROR_DQ_EMPTY`、`ERROR_RESPONSE_TIMEOUT`）：只返回错误码，不抛异常，连接保留（含 `subscribe` 不存在的 tag）
  - USAGE：抛 `GplatUsageError`，连接保留（`ERROR_BUFFER_TOO_SMALL` 时过大的包体已被读掉丢弃）
  - CONNECTION：抛 `GplatConnectionError`，封装类已关闭自己的 fd，`is_open()` 为 false
  - C 接口本身不抛异常，只通过错误钩子上报（默认写 stderr，可用 `SetErrorHook(NULL, NULL)` 关闭）并返回错误码
- **未连接**: 未 `open()` 或连接已关闭时调用，抛 `GplatConnectionError`（`ERROR_SOCKET_NOT_CONNECTED`）；不自动重连（重连会丢失订阅），需调用者重新 `open()` 并重新订阅
- **断线识别**: C 接口从不关闭 fd，遇到 CONNECTION 类错误（失败且 error 为 0 时按 `ERROR_SOCKET_NOT_CONNECTED` 处理）时只 `shutdown` 连接，封装类随即 `close()` 自己的 fd 再抛异常；其它类别不影响连接。`is_open()` 只反映本地状态，对端关闭要到下一次调用失败才能感知
- **拷贝/移动**: 不可拷贝，可移动（被移动对象变为未连接）
- **线程安全**: 不加锁，一个连接只能在一个线程中使用
- **示例/测试**: `testapp6`

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
char tagname[GPLAT_TAGNAME_SIZE];
char value[4096];
while (true) {
    if (waitpostdata(sockfd, tagname, sizeof(tagname), value, sizeof(value), 5000, &error)) {
        printf("Event: %s\n", tagname);
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
