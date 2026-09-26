# CLAUDE.md

## Project Overview

gPlat: Linux real-time data middleware server providing IPC via:
- **Board**: shared-memory (mmap) key-value store of named "tags" (binary, string, reflected user structs)
- **Queue**: FIFO record queues (shift / normal mode)

Plus pub/sub (delayed post, periodic timers 500ms–5s), TCP access (port 8777, custom binary protocol), and Siemens S7 PLC bridge (Snap7).

## Build

- **Makefile** (repo root, g++ C++17, Linux only): `make` / `make <target>` / `make clean-<target>` / `make help`. Outputs: `bin/`, `lib/` (`libhigplat.so`, `libsnap7.so`), `build/` (.o/.d). Snap7 built via `snap7/build/linux` upstream makefile. Binaries use rpath `$ORIGIN/../lib`.
- **Visual Studio** `gPlat.sln` (13 `.vcxproj`, ApplicationType=Linux), authored on Windows, remote-built on Linux.

Runtime paths are relative to `bin/`: config `../config/gplat.conf`, QBD files `../qbdfile`, logs `../logs`, s7ioserver config `../config/s7ioserver.ini` (`-c` to override).

## Server Architecture (gplat/, Nginx-inspired)

- Master/worker processes (`ngx_master_process_cycle()`), `socketpair()` for exit signaling; daemon via `ngx_daemon()`
- Epoll (LT) event loop; connection pool of `ngx_connection_s` with free-list and delayed recycling (`Sock_RecyConnectionWaitTime`, code default 60s)
- `CThreadPool` consumes a message queue (pthread mutex/condvar); dedicated send thread dequeues `m_MsgSendQueue` via semaphore, EPOLLOUT fallback on partial sends
- `TimerManager` (`include/timer_manager.h`): epoll + timerfd + min-heap, drift compensation
- Singletons with nested `CGarhuishou` destructor: `CConfig`, `CMemory`. Globals in `nginx.cxx`: `g_socket` (CLogicSocket), `g_threadpool`, `g_tm`
- `ngx_worker_process_init`: creates thread pool, starts 5 timers (`timer_500ms`, `timer_1s`, `timer_2s`, `timer_3s`, `timer_5s`) calling `NotifyTimerSubscriber()`, inits epoll and send/recycle threads
- **Dispatch**: `statusHandler[]` in `ngx_c_slogic.cxx`, indexed by `MSGID`; indices 0–4 NULL, 18 active handlers, rest `noop`
- **Pub/Sub** (`CSubscribe`): `std::map<std::string, std::list<EventNode>>` + `std::shared_mutex`; events DEFAULT=1, POST_DELAY=2, NOT_EQUAL_ZERO=4, EQUAL_ZERO=8; separate `m_mapSubject_plcIoServer` (latest PLC I/O server per tag)

## Wire Protocol

`MSGHEAD` (`#pragma pack(1)`) + body (≤ `MAXMSGLEN` = 16384). `MSGID` in `include/msg.h`: 49 codes, `SUCCEED = 5` … `CREATEQUEUE = 53`. Adding a message type (see `Doc/add_message_type.md`):
1. Append enum value to `MSGID`
2. Implement handler in `gplat/ngx_c_slogic.cxx`
3. Register in `statusHandler[]` at the matching index
4. Add client API in `higplat/higplat.cpp`, declare in `include/higplat.h`

## Struct Reflection (Board tags with typed display in toolgplat)

1. Define struct in `include/user_types.h` inside `#pragma pack(push, 8)` / `#pragma pack(pop)`; fields may be scalars, `PodString<N>`, fixed arrays, or one layer of nested struct. Must be trivially copyable (`static_assert` in `REGISTER_STRUCT`).
2. Register: `REGISTER_STRUCT(MyStruct, FIELD_DESC(Int32, MyStruct, value), FIELD_DESC_STRING(MyStruct, name), FIELD_DESC_ARRAY(Single, MyStruct, data, 4))`
3. Add `REG(MyStruct),` to `GetStructRegistry()` in `include/struct_registry.h`.

## s7ioserver (S7 PLC ↔ Board)

- Read thread per PLC: polls DBs, detects changes by raw byte compare, writes Board via `write_plc_*`
- Shared write thread: subscribes tags, `waitpostdata()`, writes back to PLC via Snap7
- INI config: `[general]` (gPlat connection) + per-PLC sections with tag mappings (sample: `Doc/s7ioserver.ini`)

## Network API (`extern "C"` in `include/higplat.h`, blocking TCP)

- Connection: `connectgplat(server, port)` → fd (2s timeout, TCP_NODELAY), `disconnectgplat`
- Queue: `readq`, `writeq`, `clearq`, `createqueue`
- Board: `readb` (optional timestamp), `writeb`, `writeb_notpost`, `readb_string`/`readb_string2` (std::string), `writeb_string`/`writeb_string2`, `createtag` (optional type descriptor), `deletetag`, `clearb`, `readtype`, `readboardinfo`
- Pub/Sub: `subscribe` (DEFAULT), `subscribedelaypost` (POST_DELAY), `waitpostdata` (tagname `"WAIT_TIMEOUT"` on timeout)
- PLC: `write_plc_{string,bool,short,ushort,int,uint,float}`, `registertag`

Full reference: `Doc/api_reference.md`; error codes: `Doc/ERROR_CODE.md`.

## Local API (direct mmap on QBD files, `higplat/higplat.cpp`)

- Board: `CreateB`, `CreateItem`, `DeleteItem`, `ReadB`, `WriteB`, `ReadB_String`, `WriteB_String`, `WriteBOffSet`, `ClearB`, `ReadInfoB`, `ReadBoardInfo`, `ReadType`
- Queue: `CreateQ`, `ReadQ`, `WriteQ`, `ClearQ`, `PeekQ`, `IsEmptyQ`, `IsFullQ`, `MulReadQ`, `MulReadQ2`, `SetPtrQ`, `PopJustRecordFromQueue`, `ReadHead`
- Lifecycle: `SetQbdPath`, `LoadQ`, `UnloadQ`, `UnloadAll`, `FlushQFile`

**Board locking** (`higplat/qbd.h`): global `std::mutex mutex_rw` for hash index traversal + striped `mutex_rw_tag[MUTEXSIZE=64]` (by tag hash) for data access; placement-new'd into the mmap'd file.

## Dependencies & Conventions

- System: pthread, epoll, timerfd, eventfd, mmap, POSIX sockets/signals, `std::filesystem`; libreadline (toolgplat), libsnap7 (s7ioserver)
- All executables link `libhigplat.so`
- Server RAII lock: `CLock` (`gplat/ngx_c_lockmutex.h`) over `pthread_mutex_t`

## Projects

| Dir | Description |
|---|---|
| `gplat/` | Server. Entry `nginx.cxx`; network `ngx_c_socket*`; logic `ngx_c_slogic.*`; `CSubscribe.*` |
| `higplat/` | `libhigplat.so`: local mmap QBD ops + network client |
| `include/` | Shared headers (`higplat.h`, `msg.h`, `timer_manager.h`, `podstring.h`, `type_code.h`, `struct_reflect.h`, `struct_registry.h`, `user_types.h`); also a header-compile test project |
| `createq/`, `createb/` | CLI to create Queue / Board files |
| `toolgplat/` | Interactive REPL client (readline, type-aware display) |
| `testapp/` | Integration test (subscribe/read/write threads) |
| `testapp2/` | Stress test (10 threads × 100 tags, subscribe chains, large data) |
| `testapp3/` | Struct type test (`PodString`, arrays, nested) |
| `testapp4/` | Subscribe/`waitpostdata` test |
| `s7ioserver/` | PLC ↔ Board bridge |
| `snap7/` | Snap7 source (`libsnap7.so`) |
| `snap7.demo.cpp/` | Snap7 demo (VS only, not in Makefile) |