# gPlat 架构文档

## 概述

gPlat 是一个高性能的实时数据平台，采用 Nginx 启发的主从进程架构，专为工业自动化和分布式系统的进程间通信（IPC）而设计。它提供队列（Queue）、公告板（Board）和数据库（Database）三种核心数据结构，支持低延迟、高并发的数据交换。

---

## 项目结构

```
gPlat/
├── gplat/              # 核心服务器应用（主程序）
├── higplat/            # 共享库（QBD 数据管理系统）
├── include/            # 共享头文件和公共接口
├── testapp/            # 测试客户端应用
├── createq/            # 队列创建工具
├── createb/            # 公告板创建工具
├── toolgplat/          # 通用平台工具
├── Doc/                # 文档目录
├── readme/             # 快速入门指南
└── x64/                # 编译输出目录
```

**项目规模:**
- 6 个 Visual Studio C++ 工程
- 48 个源文件（C++/CXX/H）
- 支持多平台（x86, x64, ARM, ARM64）

---

## 技术栈

### 编程语言与标准
- **C++17**: 核心开发语言
- **C**: 部分底层操作

### 核心技术
| 技术领域 | 使用技术 |
|---------|---------|
| **网络通信** | POSIX Sockets, Epoll (Linux I/O 多路复用) |
| **线程模型** | POSIX Threads (pthread), C++11 atomics, mutex, condition_variable |
| **进程间通信** | Unix Domain Sockets, socketpair |
| **内存管理** | Memory-mapped files (mmap), 自定义内存池 |
| **数据序列化** | 二进制协议 + CRC32 校验 |
| **定时器** | timerfd, eventfd, 自定义 TimerManager |

### 平台与构建
- **目标平台**: Linux（从 Windows 进行远程开发）
- **开发环境**: Visual Studio 2022 + Remote Linux Development
- **构建系统**: CMake + Visual Studio 项目文件
- **最低 CMake 版本**: 3.12

---

## 核心架构

### 1. 主从进程模型（Master-Worker Architecture）

参考 Nginx 设计，实现高性能的多进程架构：

```
┌─────────────────────────────────────┐
│      Master Process                 │
│  - 读取配置                         │
│  - 创建 Worker 进程                 │
│  - 处理信号（reload, shutdown）     │
│  - 监控 Worker 健康状态             │
└──────────┬──────────────────────────┘
           │
           ├─────────────┬──────────────┬───────────
           │             │              │
      ┌────▼────┐   ┌────▼────┐   ┌────▼────┐
      │ Worker 1│   │ Worker 2│   │ Worker N│
      │ - Epoll │   │ - Epoll │   │ - Epoll │
      │ - 连接池│   │ - 连接池│   │ - 连接池│
      │ - 线程池│   │ - 线程池│   │ - 线程池│
      └─────────┘   └─────────┘   └─────────┘
```

**关键文件:**
- `nginx.cxx`: 主入口
- `ngx_process_cycle.cxx`: 进程生命周期管理
- `ngx_signal.cxx/h`: 信号处理

### 2. 网络层架构（Socket Layer）

基于 Epoll 的事件驱动模型，支持非阻塞 I/O 和高并发连接：

```
┌─────────────────────────────────────────┐
│          CSocekt (网络层基类)           │
├─────────────────────────────────────────┤
│  ngx_c_socket.cxx/h                     │
│  - Initialize(): 初始化监听套接字      │
│  - epoll_init(): 创建 epoll 实例        │
│  - ngx_epoll_process_events(): 事件循环 │
└────────┬──────────┬──────────┬──────────┘
         │          │          │
    ┌────▼────┐ ┌──▼─────┐ ┌──▼──────────┐
    │ Accept  │ │ Connect│ │   Request   │
    │ 连接接受│ │ 连接管理│ │  请求处理   │
    └─────────┘ └────────┘ └─────────────┘
```

**核心组件:**
- `ngx_c_socket_accept.cxx`: 接受新连接
- `ngx_c_socket_conn.cxx`: 连接池管理
- `ngx_c_socket_request.cxx`: 请求接收和发送
- `ngx_c_socket_inet.cxx`: 网络工具函数

**连接池设计:**
```cpp
连接生命周期:
m_connectionList  ←→  m_freeconnectionList  →  m_recyconnqueueList
(活跃连接)            (空闲连接)                (待回收连接)
```

### 3. 业务逻辑层（Logic Layer）

```
┌─────────────────────────────────────────┐
│   CLogicSocket (继承自 CSocekt)         │
├─────────────────────────────────────────┤
│  ngx_c_slogic.cxx/h                     │
│  - ThreadRecvProcFunc(): 消息处理线程   │
│  - ServerRecvQueueThread(): 接收队列    │
│  - ServerSendQueueThread(): 发送队列    │
│  - procping(): 心跳处理                 │
│  - HandleXXX(): 业务命令处理            │
└─────────────────────────────────────────┘
         ↓
    线程池处理消息
         ↓
    调用 higplat API
         ↓
    操作 QBD 数据结构
```

### 4. 数据管理层（Data Management Layer - QBD System）

gPlat 的核心数据结构，提供三种存储类型：

#### Queue（队列）
```
┌──────────────────────────────────────────┐
│           QUEUE_HEAD                     │
├──────────────────────────────────────────┤
│  qbdtype, dataType, operateMode          │
│  num, size                               │
│  readPoint, writePoint                   │
└──────────┬───────────────────────────────┘
           │
    ┌──────▼──────────────────────────┐
    │    RECORD_HEAD + Data           │
    │  [0] [1] [2] ... [num-1]        │
    └─────────────────────────────────┘
```

**特性:**
- FIFO 顺序
- 循环缓冲区实现
- 支持 SHIFT 模式（满时自动移除最旧记录）
- 记录级别的元数据（创建时间、源 IP、确认标志）

#### Board（公告板）
```
┌──────────────────────────────────────────┐
│           BOARD_HEAD                     │
├──────────────────────────────────────────┤
│  哈希索引表 (7177 slots)                 │
│  BOARD_INDEX_STRUCT[7177]                │
│  - itemname (tag 名称)                   │
│  - startpos, itemsize                    │
│  - timestamp                             │
│  - typeaddr, typesize                    │
└──────────┬───────────────────────────────┘
           │
    ┌──────▼──────────────────────────┐
    │      数据区（动态分配）         │
    │  [tag1_data][tag2_data]...      │
    └─────────────────────────────────┘
```

**特性:**
- 键值存储（tag-value）
- 哈希表索引（7177 个桶，质数减少冲突）
- 开放寻址法处理冲突
- 细粒度锁（64 个互斥锁）
- 支持动态添加/删除 tag

#### Database（数据库）
```
┌──────────────────────────────────────────┐
│             DB_HEAD                      │
├──────────────────────────────────────────┤
│  哈希索引表 (7177 slots)                 │
│  DB_INDEX_STRUCT[7177]                   │
│  - tablename                             │
│  - recordsize, maxcount, currcount       │
│  - timestamp                             │
└──────────┬───────────────────────────────┘
           │
    ┌──────▼──────────────────────────┐
    │    表数据（固定大小记录）       │
    │  [record1][record2]...           │
    └─────────────────────────────────┘
```

**特性:**
- 多表支持
- 固定大小记录
- 表级别索引和管理

**存储实现:**
- 内存映射文件（mmap）持久化
- 文件路径: `./qbdfile/[name].qbd`
- 零拷贝数据访问

### 5. 线程模型

```
主线程:
  ├─ epoll_wait() 事件循环
  └─ 接受连接、处理网络事件

消息接收线程 (1个):
  └─ ServerRecvQueueThread()
      └─ 从接收队列中取消息
          └─ 分发到线程池

线程池 (可配置数量):
  └─ ThreadRecvProcFunc()
      └─ 处理业务逻辑（MSGID 命令）

消息发送线程 (1个):
  └─ ServerSendQueueThread()
      └─ 从发送队列中取消息
          └─ 写入 socket
```

**关键文件:**
- `ngx_c_threadpool.cxx/h`: 线程池实现

---

## 设计模式

### 单例模式
```cpp
class CConfig {
    static CConfig* m_instance;
    class CGarhuishou { ~CGarhuishou() { delete m_instance; } };
    static CGarhuishou Garhuishou;
};
```
应用于: `CConfig`, `CMemory`

### 观察者模式
```cpp
class CSubscribe {
    // 订阅管理
    bool subscribeDelayPost(const char* tagName, ...);
    bool waitpostdata(std::string& tagName, ...);
};
```

### 对象池模式
- 连接池: 复用 TCP 连接对象
- 内存池: 高效内存分配

### 生产者-消费者模式
- 消息接收队列 + 信号量
- 线程池处理消息

---

## 通信协议

### 消息结构
```cpp
┌────────────────────────────────────┐
│        COMM_PKG_HEAD               │
├────────────────────────────────────┤
│  pkgLen (包长度, 2 bytes)          │
│  msgCode (消息类型, 2 bytes)       │
│  crc32 (CRC32 校验, 4 bytes)       │
└────────┬───────────────────────────┘
         │
    ┌────▼────────────────┐
    │     MSGHEAD         │
    │  - id (MSGID 枚举)  │
    │  - qname, itemname  │
    │  - datasize, etc.   │
    └────┬────────────────┘
         │
    ┌────▼────────────────┐
    │    Body Data        │
    │  (变长, 最大 128KB) │
    └─────────────────────┘
```

### 命令集（MSGID）
| 类别 | 命令 |
|------|------|
| **连接管理** | CONNECT, RECONNECT, DISCONNECT |
| **队列操作** | READQ, WRITEQ, CLEARQ, ISEMPTYQ, ISFULLQ, PEEKQ |
| **公告板操作** | READB, WRITEB, READBSTRING, WRITEBSTRING, CLEARB |
| **数据库操作** | SELECTTB, INSERTTB, CLEARTB, REFRESHTB, CREATETABLE, DELETETABLE |
| **订阅机制** | SUBSCRIBE, CANCELSUBSCRIBE, POST, POSTWAIT |
| **管理操作** | CREATEITEM, DELETEITEM, READTYPE |

---

## 基础设施组件

### 配置管理 (`ngx_c_conf.cxx/h`)
- INI 格式配置文件（nginx.conf）
- 支持的配置项:
  - `ListenPort`: 监听端口（默认 8777）
  - `WorkerProcesses`: Worker 进程数
  - `Daemon`: 守护进程模式
  - `ProcMsgRecvWorkThreadCount`: 消息处理线程数

### 内存管理 (`ngx_c_memory.cxx/h`)
```cpp
class CMemory {
    void* AllocMemory(int memCount, bool ifmemset);
    void FreeMemory(void* point);
};
```

### 日志系统 (`ngx_log.cxx`)
- 日志级别: DEBUG, INFO, NOTICE, WARN, ERROR, CRIT, ALERT, EMERG
- 日志文件: `error.log`
- 格式: 时间戳 + 日志级别 + 消息

### 定时器管理 (`timer_manager.h`)
```cpp
class TimerManager {
    int CreateTimer(int interval, TimerCallback callback, bool oneShot);
    void DeleteTimer(int timerId);
    void ProcessTimers();
};
```
- 基于 timerfd + epoll 集成
- 支持周期和一次性定时器

### 进程标题设置 (`ngx_setproctitle.cxx`)
- 动态设置进程名称
- 便于监控和管理（例如: `nginx: master process`, `nginx: worker process`）

---

## 部署架构

### 配置文件示例 (`nginx.conf`)
```ini
[Socket]
ListenPort = 8777
WorkerProcesses = 1

[Proc]
Daemon = 1
ProcMsgRecvWorkThreadCount = 4
```

### 目录结构
```
运行目录/
├── nginx.conf          # 配置文件
├── gplat               # 可执行文件
├── libhigplat.so       # 共享库
├── error.log           # 日志文件
└── qbdfile/            # QBD 数据文件目录
    ├── queue1.qbd
    ├── board1.qbd
    └── db1.qbd
```

### 守护进程模式
- 支持后台运行（Daemon=1）
- 信号处理:
  - `SIGTERM`: 优雅关闭
  - `SIGHUP`: 重新加载配置
  - `SIGQUIT`: 快速关闭

---

## 性能优化策略

### 网络层优化
1. **Epoll 边缘触发模式**: 减少系统调用
2. **非阻塞 I/O**: 避免线程阻塞
3. **TCP_NODELAY**: 禁用 Nagle 算法，降低延迟
4. **连接池**: 复用连接对象，减少分配开销

### 数据层优化
1. **内存映射文件**: 零拷贝数据访问
2. **哈希索引**: O(1) 查找性能
3. **细粒度锁**: 64 个互斥锁减少锁竞争
4. **对象池**: 减少内存分配/释放

### 并发优化
1. **主从进程模型**: 利用多核 CPU
2. **线程池**: 避免线程创建开销
3. **无锁队列**: 减少锁竞争（部分场景）

---

## 可扩展性

### 水平扩展
- 多 Worker 进程利用多核
- 每个 Worker 可配置线程池大小

### 容量限制
- 每个 Board/Database: 最大 7177 个项
- 消息大小: 最大 128KB
- 连接数: 可配置（`m_worker_connections`）

---

## 安全性考虑

1. **数据完整性**: CRC32 校验
2. **序列号验证**: 防止消息重放
3. **连接回收**: 心跳超时检测
4. **资源限制**: 连接数、消息大小限制

---

## 依赖关系

### 系统依赖
- Linux Kernel >= 2.6.27 (epoll, timerfd, eventfd)
- GCC/G++ with C++17 support
- pthread

### 无外部第三方库
- 自包含实现
- 无 Boost、Qt 等外部依赖

---

## 编译与构建

### CMake 构建
```bash
mkdir build && cd build
cmake ..
make
```

### 输出文件
- `gplat`: 服务器可执行文件
- `libhigplat.so`: 客户端共享库
- `testapp`: 测试客户端

---

## 总结

gPlat 是一个专为实时系统设计的高性能 IPC 平台，其架构特点包括:

1. **高性能**: Epoll 事件驱动 + 内存映射文件 + 零拷贝
2. **高可靠性**: 主从进程架构 + CRC32 校验 + 数据持久化
3. **高并发**: 线程池 + 连接池 + 细粒度锁
4. **易使用**: 简洁的 C API + 丰富的命令行工具
5. **灵活性**: 三种数据结构满足不同场景 + 发布订阅模式

该架构参考了 Nginx 的成熟设计，同时针对 IPC 场景进行了优化，适合工业自动化、过程控制、边缘计算等对实时性和可靠性要求高的应用场景。
