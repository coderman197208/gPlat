# 错误码系统

## 1. 背景与目标

当前 gPlat 接口采用 C 风格的 `unsigned int* error` 参数传递错误状态码，为实现以下目标，设计新的错误码系统：

- 兼容性：保持与既有的 C/C++ ABI 兼容，并且维持基础的原生指针通信签名，不修改任何现有客户端调用代码。
- 可读性：提供人类可读的错误描述，便于调试和维护。
- 错误分级：根据错误的严重程度进行分级处理。

## 2. 系统架构

### 2.1 错误码定义

底层错误码 ID 继续沿用 `#define` 宏的方式：

```cpp
#define ERROR_DQFILE_NOT_FOUND          1
#define ERROR_DQ_NOT_OPEN               2
// ...
```

### 2.2 错误分级定义

使用枚举类 `ErrorLevel` 定义错误的紧迫性，不同的紧迫性对应不同的默认响应动作：

```cpp
enum class ErrorLevel {
    Deprecated = -1, // 废弃：表示错误码已经不被使用
    Ignore = 0,      // 忽视：表示不需要特殊处理的错误，不打断程序执行流
    Fatal = 1        // 致命：将抛出异常、退出进程或程序
};
```

### 2.3 错误信息映射

使用 inline function 和 switch-case 实现 lookup table 用于将错误码映射到对应的错误信息和级别。提供 O(1) 的查询性能。

以下代码仅为示例，不代表真实映射关系：

```cpp
struct ErrorInfo {
    ErrorLevel level;
    const char* message;
};

inline ErrorInfo GetErrorInfo(unsigned int errorCode) {
    switch (errorCode) {
        case ERROR_DQFILE_NOT_FOUND:
            return { ErrorLevel::Fatal, "Data file not found" };
        case ERROR_DQ_NOT_OPEN:
            return { ErrorLevel::Ignore, "Queue not open" };
        // ...
        default:
            return { ErrorLevel::Ignore, "Unknown error" };
    }
}
```

### 2.4 拦截器

目标是实现对客户端程序的错误码进行自动监听和处理。

库维护者只需要在函数调用头部声明一个拦截器对象，该对象的析构函数会在函数调用的生命周期终结前自动执行，实现返回时自动检查错误码并处置的功能。

这种设计不需要修改函数签名，也不需要在每个调用点显式地检查错误码。

使用同线程重入计数器避免在嵌套调用中多次检查错误码，确保只有在最外层 API 调用返回时才进行错误检查。

```cpp
namespace {
    // 线程内重入深度计数器
    thread_local int g_api_depth = 0;
}

struct AutoErrorCheck {
    unsigned int* m_error;

    AutoErrorCheck(unsigned int* error) : m_error(error) {
        // 进入函数时增加深度计数器
        g_api_depth++;
    }

    ~AutoErrorCheck() noexcept(false) {
        // 离开函数时减少深度计数器
        g_api_depth--;
		// 只有当回到最外层 API 调用时，即 g_api_depth == 0 时，才检查错误码，避免在嵌套调用中多次检查
        if (g_api_depth == 0 && m_error != nullptr && *m_error != 0) {
            ErrorInfo info = GetErrorInfo(*m_error);
            if (info.level == ErrorLevel::Fatal) {
                // 致命错误处理
            } 
            else if (info.level == ErrorLevel::Deprecated) {
                // 废弃错误处理
            }
            else if (info.level == ErrorLevel::Ignore) {
                // 忽视错误处理
            }
        }
    }
};
```

客户端函数的接口维护者只需要在函数体首行声明一个 `AutoErrorCheck` 对象，原函数逻辑无需改动，错误码的监听和处理将自动进行。其原理是在函数返回前，`AutoErrorCheck` 的析构函数会被调用，从而触发错误码的检查和处理逻辑。

## 3. 使用示例

### 3.1 库实现端

接口维护者只需要在对外暴露的接口区域首行声明拦截器，原函数逻辑无需改动，以下是一个示例：

```cpp
extern "C" bool readq(int sockfd, const char* qname, void* record, int actsize, unsigned int* error) {
    // 仅需在此处使用 unsigned int* error 参数构造对象
    AutoErrorCheck _checker(error); 
    // 原有函数逻辑保持不变
    // return 调用前，_checker 的析构函数会自动触发并探查 *error
    return true; 
}
```

### 3.2 客户端

作为调用方，原有的包含 `&error` 参数的语句不需要做任何修改，错误码的监听和处理完全由库实现端的拦截器自动完成。
