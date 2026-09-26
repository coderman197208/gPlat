# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

gPlat is a real-time data platform / middleware server for Linux. It provides inter-process communication via two data models:
- **Board**: Shared-memory key-value store for named data items ("tags"), supporting binary, string, and user-defined struct data with compile-time type reflection
- **Queue**: FIFO message queues with user-defined record sizes, supporting shift mode and normal mode

Additional features include:
- **Publish-subscribe** with delayed posting, timer-based periodic events (500ms to 5s)
- **TCP network access** (default port 8777) using a custom binary protocol
- **PLC integration** via Siemens S7 protocol (Snap7 library) with a dedicated I/O server bridge
- **Compile-time struct reflection** with `PodString<N>`, array fields, and nested structs (one layer)

## Build System

Supports two build methods.

### The code is authored on Windows and cross-compiled to Linux via Visual Studio's "Visual C++ for Linux Development" workload.
Visual Studio solution `gPlat.sln` (13 projects) with `.vcxproj` projects targeting Linux (ApplicationType = Linux). 

### Makefile build
Run the `make` command in the `gPlat` directory.

## Architecture

The server follows an **Nginx-inspired architecture**:
- **Master/worker process model** via `ngx_master_process_cycle()` with `socketpair()` IPC for exit signaling
- **Epoll-based event loop** (LT mode) for non-blocking I/O
- **Connection pool** with pre-allocated `ngx_connection_s` objects managed via free-list, delayed recycling (60s default)
- **Thread pool** (`CThreadPool`) consuming from a message queue with pthread mutex/condvar
- **Dedicated send thread** dequeuing from `m_MsgSendQueue` via semaphore, with EPOLLOUT fallback for partial sends
- **Configuration** read from `nginx.conf` via `CConfig` singleton
- **Timer manager** (`TimerManager` in `timer_manager.h`) using epoll + timerfd + min-heap with drift compensation
- **Daemon mode** via `ngx_daemon()`

**Singletons** (all use nested `CGarhuishou` destructor pattern): `CConfig`, `CMemory`.

**Global objects** (defined in `nginx.cxx`): `g_socket` (CLogicSocket), `g_threadpool` (CThreadPool), `g_tm` (TimerManager).

**Message dispatching**: Function pointer array `statusHandler[]` in `ngx_c_slogic.cxx`, indexed by `MSGID` enum values (5–51) from `msg.h`. 17 active handlers, rest are `noop`.

**Pub/Sub**: `CSubscribe` uses `std::map<std::string, std::list<EventNode>>` with `std::shared_mutex` for thread-safe subscriber management. Event types: DEFAULT, POST_DELAY, NOT_EQUAL_ZERO, EQUAL_ZERO. Separate map for PLC I/O server subscribers.

**Worker initialization** (in `ngx_worker_process_init`): Creates thread pool, starts TimerManager with 5 periodic timers (`timer_500ms`, `timer_1s`, `timer_2s`, `timer_3s`, `timer_5s`) that fire `NotifyTimerSubscriber()`, initializes epoll and send/recycle threads.

## Wire Protocol

Messages use `MSGHEAD` (packed struct, `#pragma pack(1)`) as header followed by a variable-length body (max 16KB per `MAXMSGLEN`). The `MSGID` enum defines 47 operation codes (values 5–51). Adding a new message type requires:
1. Add enum value to `MSGID` in `include/msg.h`
2. Implement handler in `gplat/ngx_c_slogic.cxx`
3. Register handler in `statusHandler[]` array (index must match enum value)
4. Add client-side API in `higplat/higplat.cpp` and declare in `include/higplat.h`

## Compile-Time Struct Reflection System

A macro-based reflection system allows user-defined structs to be stored in Board tags with field-level type-aware display in `toolgplat`.

### Adding a New User-Defined Struct

1. **Define the struct** in `include/user_types.h` with `#pragma pack(push, 8)`:
   ```cpp
   #pragma pack(push, 8)
   struct MyStruct {
       int32_t       value;
       PodString<20> name;
       float         data[4];
   };
   #pragma pack(pop)
   ```

2. **Register with reflection macros**:
   ```cpp
   REGISTER_STRUCT(MyStruct,
       FIELD_DESC(Int32,   MyStruct, value),
       FIELD_DESC_STRING(MyStruct, name),
       FIELD_DESC_ARRAY(Single, MyStruct, data, 4)
   )
   ```

3. **Add to global registry** in `include/struct_registry.h`:
   ```cpp
   REG(MyStruct),
   ```

## S7 PLC Integration

The `s7ioserver` bridges Siemens S7 PLCs with gPlat Board storage using the Snap7 library.

### Architecture

- **Read threads** (one per PLC): Poll PLC data blocks at configurable intervals, detect changes via raw byte comparison, write changed values to Board tags via `write_plc_*` API
- **Write thread** (shared): Subscribes to Board tags, receives change notifications via `waitpostdata()`, writes values back to PLC via Snap7
- **Configuration**: INI-style file (`s7ioserver.ini`) with `[general]` section for gPlat connection and per-PLC sections defining tag mappings

## Network API (Client → Server)

All `extern "C"` functions in `higplat.h`. Each uses blocking TCP with `MSGHEAD` send/recv.

### Connection
- `connectgplat(server, port)` → socket fd (2s timeout, TCP_NODELAY)
- `disconnectgplat(sockfd)`

### Queue Operations
- `readq`, `writeq`, `clearq`

### Board Operations (Binary)
- `readb` (with optional timestamp), `writeb`

### Board Operations (String)
- `readb_string` (char buffer), `readb_string2` (std::string)
- `writeb_string` (C string), `writeb_string2` (std::string)

### Board Management
- `createtag` (with optional type descriptor), `deletetag`, `clearb`
- `readtype` — Read type descriptor for a tag

### Pub/Sub
- `subscribe` — Subscribe to tag change (DEFAULT event)
- `subscribedelaypost` — Subscribe with POST_DELAY event and delay time
- `waitpostdata` — Blocking wait for posted data (with timeout; returns `"WAIT_TIMEOUT"` on timeout)

## Local API (Direct mmap)

Used by the server internally and by co-located processes. Operates directly on memory-mapped QBD files.

### Board: `CreateB`, `CreateItem`, `DeleteItem`, `ReadB`, `WriteB`, `ReadB_String`, `WriteB_String`, `WriteBOffSet`, `ClearB`, `ReadInfoB`, `ReadType`
### Queue: `CreateQ`, `LoadQ`, `UnloadQ`, `ReadQ`, `WriteQ`, `ClearQ`, `PeekQ`, `IsEmptyQ`, `IsFullQ`, `MulReadQ`, `MulReadQ2`, `SetPtrQ`, `PopJustRecordFromQueue`, `ReadHead`
### Lifecycle: `LoadQ`, `UnloadQ`, `UnloadAll`, `FlushQFile`

## Board Locking Model

Two-level locking for concurrent access:
1. **Global mutex** (`BOARD_HEAD.mutex_rw`): Acquired for hash index traversal
2. **Per-tag striped mutex** (`BOARD_HEAD.mutex_rw_tag[hash & 63]`): Acquired for data read/write

64 stripe locks (MUTEXSIZE) allow concurrent access to different tags. Mutexes are placement-new'd into the mmap'd file for cross-process sharing.

## Dependencies

- **System**: pthread, Linux epoll, timerfd, eventfd, mmap, POSIX sockets/signals, `std::filesystem`
- **External**: libreadline (toolgplat only), libsnap7 (s7ioserver only)
- **Internal**: All executables link `libhigplat.so`; s7ioserver additionally links `libsnap7.so`

## Code Conventions

- RAII locking via `CLock` wrapper around `pthread_mutex_t`
- User-defined structs must be `trivially_copyable` (enforced by `static_assert` in `REGISTER_STRUCT`)

## Solution Projects (13)

| Project | Type | Directory | Description |
|---|---|---|---|
| gplat | Executable | `gplat/` | Server |
| higplat | Shared library | `higplat/` | Client library (libhigplat.so) |
| include | Test executable | `include/` | Header compilation test |
| createq | Executable | `createq/` | Queue creation tool |
| createb | Executable | `createb/` | Board creation tool |
| toolgplat | Executable | `toolgplat/` | Interactive REPL client |
| testapp | Executable | `testapp/` | Subscribe/read/write integration test |
| testapp2 | Executable | `testapp2/` | Performance stress test |
| testapp3 | Executable | `testapp3/` | Struct type test |
| s7ioserver | Executable | `s7ioserver/` | PLC-to-Board bridge |
| snap7 | Shared library | `snap7/` | Snap7 PLC library (libsnap7.so) |
| snap7.demo.cpp | Executable | `snap7.demo.cpp/` | Snap7 demo |

- **gplat/** — Server executable. Entry point: `nginx.cxx`. Networking: `ngx_c_socket*`. Business logic: `ngx_c_slogic.*`. Subscribe engine: `CSubscribe.*`.
- **higplat/** — Client shared library (`libhigplat.so`). Contains both local mmap-based QBD operations and network client API. Public API declared with `extern "C"` in `include/higplat.h`.
- **include/** — Shared headers. `higplat.h` (public API + data structures), `msg.h` (wire protocol), `timer_manager.h` (timer), `podstring.h` (PodString<N>), `type_code.h` / `struct_reflect.h` / `struct_registry.h` / `user_types.h` (struct reflection system).
- **createq/, createb/** — CLI tools to create Queue and Board data files on disk.
- **toolgplat/** — Interactive REPL client using libreadline with type-aware tag display.
- **testapp/** — Multi-threaded integration test (3 threads: subscribe, read, write).
- **testapp2/** — Performance and correctness stress test (10 threads × 100 tags, subscribe chain propagation, large data transfers).
- **testapp3/** — Struct type test exercising `PodString`, float arrays, string arrays, nested structs.
- **s7ioserver/** — Siemens S7 PLC I/O bridge. Reads PLC data via Snap7, writes to Board tags, supports PLC write-back via gPlat subscribe mechanism.
- **snap7/** — Snap7 library source (builds `libsnap7.so`). Siemens S7 communication protocol implementation.
- **snap7.demo.cpp/** — Snap7 standalone demo application.