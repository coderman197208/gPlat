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
- `CThreadPool` consumes a message queue (pthread mutex/condvar)
- **Send path** (`msgSend()`, any thread): under per-connection `sendMutex`, sends directly (non-blocking, `MSG_NOSIGNAL`) when the connection has no backlog (`iSendQueued == 0 && iThrowsendCount == 0`); otherwise queues to `m_MsgSendQueue` for the send thread, which swaps the queue out and sends without holding the queue lock, deferring a connection's messages while it waits for EPOLLOUT. Partial send → remainder via EPOLLOUT (`ngx_write_request_handler`). `close(fd)` + `++iCurrsequence` happen under `sendMutex` (`ngx_close_and_recycle`). Lock order: `logicPorcMutex` → `sendMutex` → `m_sendMessageQueueMutex`
- `TimerManager` (`include/timer_manager.h`): epoll + timerfd + min-heap, drift compensation
- Singletons with nested `CGarhuishou` destructor: `CConfig`, `CMemory`. Globals in `nginx.cxx`: `g_socket` (CLogicSocket), `g_threadpool`, `g_tm`
- `ngx_worker_process_init`: creates thread pool, starts 5 timers (`timer_500ms`, `timer_1s`, `timer_2s`, `timer_3s`, `timer_5s`) calling `NotifyTimerSubscriber()`, inits epoll and send/recycle threads
- **Dispatch**: `statusHandler[]` in `ngx_c_slogic.cxx`, indexed by `MSGID`; indices 0–4 NULL, 23 active handlers, rest `noop`
- **Pub/Sub** (`CSubscribe`): `std::map<std::string, std::list<EventNode>>` + `std::shared_mutex`; events DEFAULT=1, POST_DELAY=2, NOT_EQUAL_ZERO=4, EQUAL_ZERO=8; separate `m_mapSubject_plcIoServer` (latest PLC I/O server per tag). `Attach` dedups per connection (DEFAULT keyed by connection+event; others also by eventname/eventarg). Storm protection: each connection's pending-event queue `m_listPost` is capped (`Sock_MaxPendingPost`, default 1000) via `ngx_connection_s::EnqueuePost` (caller holds `logicPorcMutex`); when full the newest event is dropped with a rate-limited log; no subscriber-count limit
- **Request/Response** (`HandleGetResponse` in `ngx_c_slogic.cxx`): `m_mapReqChannel` (request_tag → active + waiting deque, max 64) and `m_mapResponseOwner` (response_tag → request_tag) under `m_reqMutex`; never take `logicPorcMutex` while holding it. `HandleWriteB` (start==1) routes known response_tags to `DeliverResponse` instead of `NotifySubscriber`. Per-request timeout via `g_tm`; `CancelRequest()` on disconnect

## Wire Protocol

`MSGHEAD` (`#pragma pack(1)`) + body (≤ `MAXMSGLEN` = 16384). `MSGID` in `include/msg.h`: 52 codes, `SUCCEED = 5` … `LISTTAGS = 56`. Adding a message type (see `Doc/add_message_type.md`):
1. Append enum value to `MSGID`
2. Implement handler in `gplat/ngx_c_slogic.cxx`
3. Register in `statusHandler[]` at the matching index
4. Add client API in `higplat/higplat.cpp`, declare in `include/higplat.h` (C types only, no default args, must not throw); optionally wrap in `include/gplat_connection.h`

## Struct Reflection (Board tags with typed display in toolgplat)

1. Define struct in `include/user_types.h` inside `#pragma pack(push, 8)` / `#pragma pack(pop)`; fields may be scalars, `PodString<N>`, fixed arrays, or one layer of nested struct. Must be trivially copyable (`static_assert` in `REGISTER_STRUCT`).
2. Register: `REGISTER_STRUCT(MyStruct, FIELD_DESC(Int32, MyStruct, value), FIELD_DESC_STRING(MyStruct, name), FIELD_DESC_ARRAY(Single, MyStruct, data, 4))`
3. Add `REG(MyStruct),` to `GetStructRegistry()` in `include/struct_registry.h`.

## s7ioserver (S7 PLC ↔ Board)

- Read thread per PLC: polls DBs, detects changes by raw byte compare, writes Board via `write_plc_*`
- Shared write thread: subscribes tags, `waitpostdata()`, writes back to PLC via Snap7
- INI config: `[general]` (gPlat connection) + per-PLC sections with tag mappings (sample: `Doc/s7ioserver.ini`)

## Network API (pure C header `include/higplat.h`, blocking TCP)

`higplat.h` compiles as C99+ and C++ (`extern "C"` block under `__cplusplus`): C types only, no default args/templates/`std::string`, never throws (null `error` → return false). `higplat/qbd.h` includes it, so `higplat.cpp` definitions are checked against the declarations. Constants `GPLAT_MAX_DATA_SIZE` (=MAXMSGLEN), `GPLAT_TAGNAME_SIZE` (=40, static_assert'ed); `GetErrorCategory(error, &msg)`; `IsFatalError` kept for compatibility (== USAGE).

**Error categories** (`GetErrorInfo` in `qbd.h`, one explicit case per code; see `Doc/ERROR_CODE.md`): `GPLAT_ERRCAT_RESULT` (not exist, empty/full, wait/response timeout, capacity — caller branches on the code), `GPLAT_ERRCAT_USAGE` (caller bug: invalid parameter, size mismatch, buffer too small), `GPLAT_ERRCAT_CONNECTION` (errno `< MY_ERR_OFFSET`, `ERROR_SOCKET_NOT_CONNECTED`, `ERROR_INVALID_RESPONSE`). New codes must get a case and a category.

**Error reporting**: each exported network function (never internal helpers like `writeb_`/`writeb_plc`) declares `AutoErrorCheck _checker(error, __func__)`; on return with a non-zero code it calls the error hook once with the category. `SetErrorHook(hook, user)` replaces it (NULL = silent); the default writes USAGE/CONNECTION as one stderr line `[higplat usage] writeb: record size invalid (code 1014)`.

- Connection: `connectgplat(server, port)` → fd (2s timeout, TCP_NODELAY), `disconnectgplat`
- Queue: `readq`, `writeq`, `clearq`, `createqueue`, `readhead` (QUEUE_HEAD), `peekq` (non-consuming, `PEEK_NEXT`/`PEEK_LATEST`), `listq` (loaded queue names)
- Board: `readb` (timestamp may be NULL), `writeb`, `writeb_notpost`, `readb_string`, `writeb_string`, `writeb_string_notpost`, `createtag` (optional type descriptor), `deletetag`, `clearb`, `readtype`, `readboardinfo`, `listtags` (paged `TAG_META` + name + type descriptor; pass `*next` until -1)
- Pub/Sub: `subscribe` (DEFAULT), `subscribedelaypost` (POST_DELAY), `waitpostdata(sockfd, char* tagname, tagnamesize >= GPLAT_TAGNAME_SIZE, ...)` (tagname `"WAIT_TIMEOUT"` on timeout)
- Request/Response: `getresponse(sockfd, request_tag, req, req_size, response_tag, rsp, rsp_size, &error, timeout_ms)` — server serializes per request_tag; errors `ERROR_RESPONSE_TIMEOUT`, `ERROR_REQUEST_QUEUE_FULL`
- PLC: `write_plc_{string(const char*),bool,short,ushort,int,uint,float}`, `registertag`

Full reference: `Doc/api_reference.md`; error codes: `Doc/ERROR_CODE.md`.

## C++ Wrapper (`include/gplat_connection.h`)

`GplatConnection(server, port)`: header-only C++ layer over `higplat.h` (all C++ features — `std::string`, exceptions, default args, `=delete` overloads, `read_value<T>` — live here, in the caller's TU). `open()`/`close()`/`is_open()`; methods drop `sockfd`, return 0 or a RESULT-category code (`[[nodiscard]] unsigned int`), take `const std::string&` names; `std::string` overloads of `readb_string`/`writeb_string`/`waitpostdata`. Exceptions by category, all derived from `GplatError : std::runtime_error` (`code()`): USAGE → `GplatUsageError`; CONNECTION, calls when not open, or any failure after which the library closed the fd (e.g. `subscribe` server error, code kept) → `GplatConnectionError`, no auto-reconnect. Because C functions `close(sockfd)` internally on I/O/protocol errors, `call()` marks the fd closed via `closed_by_library` (CONNECTION codes, `ERROR_BUFFER_TOO_SMALL`, any server error for subscribe/waitpostdata) before throwing. Non-copyable, movable, not thread-safe.

## Local API (direct mmap on QBD files, `higplat/higplat.cpp`)

- Board: `CreateB`, `CreateItem`, `DeleteItem`, `ReadB`, `WriteB`, `ReadB_String`, `WriteB_String`, `WriteBOffSet`, `ClearB`, `ReadInfoB`, `ReadBoardInfo`, `ReadType`, `ListTags`
- Queue: `CreateQ`, `ReadQ`, `WriteQ`, `ClearQ`, `PeekQ`, `PeekQRecord`, `ListQ`, `IsEmptyQ`, `IsFullQ`, `MulReadQ`, `MulReadQ2`, `SetPtrQ`, `PopJustRecordFromQueue`, `ReadHead`
- Lifecycle: `SetQbdPath`, `LoadQ`, `UnloadQ`, `UnloadAll`, `FlushQFile`; `CreateAndLoadQ` (used by `HandleCreateQueue`: rejects loaded names incl. BOARD and path-like names, then loads at runtime)

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
| `include/` | Shared headers (`higplat.h`, `gplat_connection.h`, `msg.h`, `timer_manager.h`, `podstring.h`, `type_code.h`, `struct_reflect.h`, `struct_registry.h`, `user_types.h`); also a header-compile test project |
| `createq/`, `createb/` | CLI to create Queue / Board files |
| `toolgplat/` | Interactive REPL client (readline, type-aware display). Scoped commands: global / board (`open board`) / queue (`open queue <name>`: desc, peek, last, clear, write). `export script [file]` rebuilds create commands from stored types (`export.cpp`); script files accept tag and queue create lines |
| `testapp/` | Integration test (subscribe/read/write threads) |
| `testapp2/` | Stress test (10 threads × 100 tags, subscribe chains, large data) |
| `testapp3/` | Struct type test (`PodString`, arrays, nested) |
| `testapp4/` | Subscribe/`waitpostdata` test |
| `testapp5/` | `getresponse` request/response test (basic, pending events, concurrency, timeout); Makefile only |
| `testapp6/` | `GplatConnection` wrapper test (`testapp6 [ip] [port]`); Makefile only |
| `testapp7/` | Send-path test (`testapp7 [ip] [port]`, use ≥2 worker threads): small client rcvbuf/MSS forces partial sends; pipelined large READB with slow reader (EPOLLOUT/backlog/stall), repeated bursts at the buffer-full boundary (concurrent sends on one connection), POST while backlogged, disconnect while sending + fd reuse; Makefile only |
| `s7ioserver/` | PLC ↔ Board bridge |
| `snap7/` | Snap7 source (`libsnap7.so`) |
| `snap7.demo.cpp/` | Snap7 demo (VS only, not in Makefile) |