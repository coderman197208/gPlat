# 添加新消息类型的步骤

## 概述
当需要添加新的客户端-服务端通信功能时，遵循以下步骤。

## 步骤清单

### 1. 定义消息 ID
**文件**: `include/msg.h`

```cpp
enum MSGID {
    // ... 现有消息
    NEW_COMMAND = 100,  // 选择未使用的 ID（建议从 100 开始）
};
```

**注意**:
- 避免与现有 ID 冲突
- 使用有意义的命名
- 添加注释说明用途

### 2. 设计消息格式
确定消息头和消息体的数据结构：

**消息头** (MSGHEAD):
- 已有字段: id, qname, itemname, datasize, timestamp, error 等
- 选择需要使用的字段

**消息体**:
- 确定需要传输的数据
- 如果是结构体，确保字节对齐
- 考虑跨平台兼容性

### 3. 实现服务端处理函数

#### 3.1 声明处理函数
**文件**: `gplat/ngx_c_slogic.h`

```cpp
class CLogicSocket : public CSocekt {
    // ...
    void HandleNewCommand(
        lpngx_connection_t pConn,
        MSGHEAD* pMsgHeader,
        char* pPkgBody,
        unsigned short bodyLength
    );
};
```

#### 3.2 实现处理函数
**文件**: `gplat/ngx_c_slogic.cxx`

参考模板: `.claude/templates/message_handler.template.cpp`

关键点:
- 参数验证
- 调用 higplat 内部 API
- 构造响应消息
- 错误处理
- 内存管理

#### 3.3 注册消息分发
**文件**: `gplat/ngx_c_slogic.cxx`

在 `ThreadRecvProcFunc()` 函数的 switch 语句中添加:

```cpp
case MSGID::NEW_COMMAND:
    HandleNewCommand(pConn, pMsgHeader, pPkgBody, bodyLength);
    break;
```

### 4. 实现客户端 API

#### 4.1 声明 API
**文件**: `include/higplat.h`

```cpp
extern "C" bool new_command(
    int sockfd,
    const char* param,
    unsigned int* error
);
```

**注意**:
- 使用 `extern "C"` 以支持 C 调用
- 最后一个参数通常是 `unsigned int* error`
- 使用清晰的参数命名

#### 4.2 实现 API
**文件**: `higplat/higplat.cpp`

参考模板: `.claude/templates/api_function.template.cpp`

关键点:
- 构造请求消息
- 发送请求 (sendmsg)
- 接收响应 (recvmsg)
- 解析响应
- 返回结果
- 收发统一用 `send_request` / `recv_response`，响应错位用 `fail_connection(sockfd, error, ERROR_INVALID_RESPONSE)`，包体比缓冲区大用 `discard_body`；不得 `close(sockfd)`，fd 归调用方（见 `Doc/ERROR_CODE.md` 2.2）

### 5. 测试

#### 5.1 编译
```bash
cd build
cmake ..
make
```

#### 5.2 单元测试
在 `testapp/` 中添加测试代码:

```cpp
void test_new_command() {
    unsigned int error = 0;
    int sockfd = connectgplat("127.0.0.1", 8777);

    bool result = new_command(sockfd, "test_param", &error);
    printf("Result: %s, Error: %u\n", result ? "Success" : "Failed", error);

    disconnectgplat(sockfd);
}
```

#### 5.3 集成测试
- 启动服务端: `./gplat`
- 运行测试客户端: `./testapp`
- 检查日志: `tail -f error.log`

### 6. 文档更新

更新以下文档:
- `ARCHITECTURE.md`: 如果涉及架构变更
- `PRODUCT.md`: 如果是新功能特性
- `CLAUDE.md`: 如果是重要 API

### 7. 代码审查检查清单

- [ ] 消息 ID 未冲突
- [ ] 参数验证完整
- [ ] 错误处理正确
- [ ] 内存管理无泄漏
- [ ] 线程安全（如需要）
- [ ] 日志记录适当
- [ ] API 文档注释完整
- [ ] 测试用例通过
- [ ] 代码风格一致

## 常见错误

### 错误 1: 消息 ID 冲突
**现象**: 错误的处理函数被调用
**解决**: 检查 MSGID 枚举，使用未占用的 ID

### 错误 2: 内存泄漏
**现象**: 长时间运行后内存占用增加
**解决**:
- 使用 CMemory::GetInstance()->FreeMemory() 释放
- 使用 valgrind 检测

### 错误 3: CRC32 校验失败
**现象**: 客户端收到错误响应
**解决**: 确保 pkgLen 计算正确，包含完整的包头和包体

### 错误 4: 大小端问题
**现象**: 跨平台通信数据错误
**解决**: 使用网络字节序 (htonl/ntohl)

## 示例参考

查看现有实现:
- `HandleReadQ()`: 简单的读取操作
- `HandleWriteQ()`: 简单的写入操作
- `HandleSubscribe()`: 复杂的订阅逻辑
- `HandleCreateTag()`: 带参数验证的创建操作
