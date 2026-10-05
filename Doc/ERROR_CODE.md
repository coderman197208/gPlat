# 错误码系统

## 1. 背景与目标

当前 gPlat 接口采用 C 风格的 `unsigned int* error` 参数传递错误状态码，为实现以下目标，设计新的错误码系统：

- 兼容性：保持与既有的 C/C++ ABI 兼容，并且维持基础的原生指针通信签名，不修改任何现有客户端调用代码。
- 可读性：提供人类可读的错误描述，便于调试和维护。
- 错误分类：按错误类别（而不是逐个错误码）决定调用方怎么处理：只返回错误码，还是由 C++ 封装类抛异常。

## 2. 系统架构

### 2.1 错误码定义

底层错误码 ID 继续沿用 `#define` 宏的方式（`include/higplat.h`）：

```cpp
#define ERROR_DQFILE_NOT_FOUND          (MY_ERR_OFFSET + 1)
#define ERROR_DQ_NOT_OPEN               (MY_ERR_OFFSET + 2)
// ...
```

网络 API 在 send/recv 失败时直接把 `errno` 作为错误码，所以小于 `MY_ERR_OFFSET`（1000）的非 0 错误码都是 errno。

### 2.2 错误类别

错误码分为三类，枚举值取自 `higplat.h` 的公开常量 `GPLAT_ERRCAT_*`（`GetErrorCategory` 的返回值、错误钩子的 `category` 参数）：

```cpp
enum class ErrorCategory {
    Result = GPLAT_ERRCAT_RESULT,           // 0 运行结果：调用方按返回码分支
    Usage = GPLAT_ERRCAT_USAGE,             // 1 编程错误：调用方的 bug，调用前就能检查出来
    Connection = GPLAT_ERRCAT_CONNECTION    // 2 连接不可用
};
```

| 类别 | 错误码 | 默认钩子 | GplatConnection |
|---|---|---|---|
| Result | `ERROR_TAG_NOT_EXIST`、`ERROR_ITEM_NOT_EXIST`、`ERROR_RECORD_NOT_EXIST`、`ERROR_TABLE_NOT_EXIST`、`ERROR_DQ_EMPTY`、`ERROR_DQ_FULL`、`ERROR_WAIT_TIMEOUT`、`ERROR_RESPONSE_TIMEOUT`、`ERROR_REQUEST_QUEUE_FULL`、`ERROR_ALREADY_LOAD`、`ERROR_ITEM_ALREADY_EXIST`、`ERROR_TABLE_ALREADY_EXIST`、`ERROR_FILE_IN_USE`、`ERROR_FILE_CREATE_FAILSURE`、`ERROR_NO_SPACE`、`ERROR_TABLE_OVERFLOW`、`ERROR_ITEM_OVERFLOW`，以及已不再产生的错误码和未知错误码 | 不输出 | 返回错误码 |
| Usage | `ERROR_INVALID_PARAMETER`、`ERROR_PARAMETER_SIZE`、`ERROR_RECORDSIZE`、`STRING_TOO_LONG`、`BUFFER_TOO_SMALL`、`ERROR_BUFFER_TOO_SMALL`、`ERROR_FILENAME_TOO_LONG`、`ERROR_STARTPOSITION`、`ERROR_OPERATE_PROHIBIT`、`ERROR_TABLE_ROWID` | 写 stderr | 抛 `GplatUsageError` |
| Connection | errno（`< MY_ERR_OFFSET`）、`ERROR_SOCKET_NOT_CONNECTED`、`ERROR_INVALID_RESPONSE` | 写 stderr | 抛 `GplatConnectionError` |

两个"缓冲区太小"错误码都属于 Usage，但含义不同：`BUFFER_TOO_SMALL`（1038，"string longer than buffer"）是服务端 `ReadB_String` 发现字符串比调用方缓冲区长，连接保留；`ERROR_BUFFER_TOO_SMALL`（1041，"data larger than buffer"）是数据比调用方缓冲区大，网络 API 在客户端发现时会关闭 `sockfd`。

### 2.3 错误信息映射

`higplat/qbd.h` 的 `GetErrorInfo` 用 switch-case 把错误码映射到类别和描述，O(1) 查询；每个错误码都有显式的 case。导出的 `GetErrorCategory(error, &message)` 和 `IsFatalError(error, &message)`（兼容旧接口，等价于类别为 Usage）都基于它。

```cpp
struct ErrorInfo {
    ErrorCategory category;
    const char* message;
};

inline ErrorInfo GetErrorInfo(unsigned int errorCode) {
    if (errorCode != 0 && errorCode < MY_ERR_OFFSET)
        return { ErrorCategory::Connection, ErrnoMessage((int)errorCode) };   // strerror_r 文本
    switch (errorCode) {
        case ERROR_INVALID_PARAMETER:
            return { ErrorCategory::Usage, "invalid parameter" };
        case ERROR_TAG_NOT_EXIST:
            return { ErrorCategory::Result, "tag not exist" };
        // ...
        default:
            return { ErrorCategory::Result, "unknown error" };
    }
}
```

errno 的描述放在线程局部缓冲区里，在本线程下次查询前有效。

### 2.4 拦截器与错误钩子

目标是实现对客户端程序的错误码进行自动监听和处理。

库维护者只需要在对外导出的网络 API 函数头部声明一个拦截器对象，该对象的析构函数会在函数返回前自动执行：错误码非 0 时，把错误码、类别、描述和 API 函数名（`__func__`）交给错误钩子。

这种设计不需要修改函数签名，也不需要在每个调用点显式地检查错误码。

拦截器只放在导出函数里，内部 helper（如 `writeb_`、`writeb_plc`）不放，所以每次 API 调用至多上报一次，不需要重入计数。

```cpp
namespace {
    void DefaultErrorHook(unsigned int error, int category, const char* message, const char* func, void*);
    std::mutex g_errorHookMutex;
    GPLAT_ERROR_HOOK g_errorHook = DefaultErrorHook;
    void* g_errorHookUser = nullptr;
}

struct AutoErrorCheck {
    unsigned int* m_error;
    const char* m_func;

    AutoErrorCheck(unsigned int* error, const char* func) : m_error(error), m_func(func) {}

    ~AutoErrorCheck() {
        if (m_error == nullptr || *m_error == 0)
            return;
        // 加锁复制钩子和 user，解锁后再调用；调用前后保存/恢复 errno
        ErrorInfo info = GetErrorInfo(*m_error);
        hook(*m_error, static_cast<int>(info.category), info.message, m_func, user);
    }
};
```

默认钩子 `DefaultErrorHook` 只处理 Usage 和 Connection 类：先用 `snprintf` 拼好整行，再一次 `fputs` 写到 stderr，多线程输出不会交错：

```
[higplat usage] writeb: record size invalid (code 1014)
[higplat connection] readb: Connection reset by peer (code 104)
```

调用方用 `SetErrorHook(hook, user)` 替换默认钩子（例如接入自己的日志），传 NULL 关闭输出。钩子会收到所有非 0 错误码（含 Result 类，如 `waitpostdata` 超时），由钩子自己按 `category` 过滤；钩子可能在多个线程上并发执行，不得抛异常。

## 3. 使用示例

### 3.1 库实现端

接口维护者只需要在对外导出的网络 API 函数首行声明拦截器，原函数逻辑无需改动；转调内部 helper 的导出函数（如 `writeb` → `writeb_`）在导出函数里声明，helper 里不声明。以下是一个示例：

```cpp
extern "C" bool readq(int sockfd, const char* qname, void* record, int actsize, unsigned int* error) {
    // 仅需在此处用 error 参数和函数名构造对象
    AutoErrorCheck _checker(error, __func__); 
    // 原有函数逻辑保持不变
    // return 调用前，_checker 的析构函数会自动触发并探查 *error
    return true; 
}
```

新增错误码时，在 `GetErrorInfo` 里按 2.2 的规则加 case 并选定类别：调用方能在调用前避免的归 Usage；表示连接已不可用的归 Connection；其余（目标不存在、空/满、超时、资源不足等）归 Result。

### 3.2 C 调用方

作为调用方，原有的包含 `&error` 参数的语句不需要做任何修改。未设置钩子时，Usage 和 Connection 类错误由默认钩子写到 stderr；需要接入自己的日志或关闭输出时调用 `SetErrorHook`：

```c
static void my_hook(unsigned int error, int category, const char* message, const char* func, void* user)
{
    if (category != GPLAT_ERRCAT_RESULT)
        my_log("%s: %s (code %u)", func, message, error);
}

SetErrorHook(my_hook, NULL);    // 接入自己的日志
SetErrorHook(NULL, NULL);       // 或者关闭库的错误输出
```

`higplat.h` 是纯 C 接口，异常不得穿过库边界，因此拦截器只上报不抛异常。

### 3.3 C++ 调用方（GplatConnection）

`GplatConnection` 在调用方的编译单元里按 `GetErrorCategory` 的类别决定是否抛异常：

```cpp
class GplatError : public std::runtime_error {      // what() 为 "<描述> (Code: N)"
public:
    unsigned int code() const noexcept;
};
class GplatUsageError : public GplatError {};       // Usage
class GplatConnectionError : public GplatError {};  // 连接不可用，已标记为关闭
```

- Result：方法返回错误码（0 为成功），不抛异常，连接保留。
- Usage：抛 `GplatUsageError`。连接一般保留；`ERROR_BUFFER_TOO_SMALL` 时库已关闭 socket，`is_open()` 为 false。
- Connection：抛 `GplatConnectionError`，连接已标记为关闭。未 `open()`、或连接已关闭后再调用，同样抛 `ERROR_SOCKET_NOT_CONNECTED`。
- `subscribe` / `waitpostdata` 遇到服务端错误时（`ERROR_INVALID_PARAMETER`、`ERROR_WAIT_TIMEOUT` 除外）库会关闭 socket，此时即使错误码属于 Result（如 `ERROR_TAG_NOT_EXIST`），也抛 `GplatConnectionError`，`code()` 保留原错误码。

```cpp
GplatConnection conn("127.0.0.1", 8777);
while (running) {
    try {
        if (!conn.open()) { sleep_a_while(); continue; }
        (void)conn.subscribe("alarm");      // 成功返回 0；服务端错误会关闭连接，抛 GplatConnectionError
        for (;;) {
            unsigned int err = conn.waitpostdata(tag, buf, sizeof(buf), 1000);
            if (err == ERROR_WAIT_TIMEOUT)
                continue;
            // 处理事件
        }
    }
    catch (const GplatConnectionError& e) {     // 断线：重新 open() 并重新订阅
        log("connection lost: %s", e.what());
    }
}
```

`GplatUsageError` 表示调用方代码有 bug，通常不应在业务循环里捕获。

## 4. 请求-响应错误码

| 错误码 | 值 | 类别 | 说明 |
|---|---|---|---|
| `ERROR_RESPONSE_TIMEOUT` | 1044 | Result | `getresponse` 在 `timeout_ms` 内未收到响应（含服务端排队时间） |
| `ERROR_REQUEST_QUEUE_FULL` | 1045 | Result | 同一 `request_tag` 排队的请求已达上限（64） |

参数非法（含 `response_tag` 被多个 `request_tag` 共用）返回 `ERROR_INVALID_PARAMETER`（Usage）；tag 不存在沿用 `ERROR_TAG_NOT_EXIST`（Result），大小不符沿用 `ERROR_RECORDSIZE`（Usage）。
