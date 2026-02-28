# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

gPlat is a real-time data platform / middleware server for Linux. It provides inter-process communication via three data models:
- **Board**: Shared-memory key-value store for named data items ("tags"), supporting binary, string, and user-defined struct data with compile-time type reflection
- **Queue**: FIFO message queues with configurable record sizes, supporting shift mode and normal mode
- **Database (DB)**: Table-based storage (partially implemented)

Additional features include:
- **Publish-subscribe** with delayed posting, timer-based periodic events (500ms to 5s)
- **TCP network access** (default port 8777) using a custom binary protocol
- **PLC integration** via Siemens S7 protocol (Snap7 library) with a dedicated I/O server bridge
- **Compile-time struct reflection** with `PodString<N>`, array fields, and nested structs (one layer)

## Build System

The code is authored on Windows and **cross-compiled to Linux** via Visual Studio 2022's "Visual C++ for Linux Development" workload.

**Primary build**: Visual Studio solution `gPlat.sln` (13 projects) with `.vcxproj` projects targeting Linux (ApplicationType = Linux). Remote root directory: `~/projects`.

**CMake build** (gplat server only):
```bash
cd gplat
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make
```

**Post-build**: `higplat` copies `libhigplat.so` to `/usr/local/lib/` and runs `ldconfig`.

**C++ standard**: C++17 for gplat, higplat, and toolgplat; C++11 for testapp.

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

**Singletons** (all use nested `CGarhuishou` destructor pattern): `CConfig`, `CMemory`, `CCRC32`.

**Global objects** (defined in `nginx.cxx`): `g_socket` (CLogicSocket), `g_threadpool` (CThreadPool), `g_tm` (TimerManager).

**Message dispatching**: Function pointer array `statusHandler[]` in `ngx_c_slogic.cxx`, indexed by `MSGID` enum values (5–51) from `msg.h`. 17 active handlers, rest are `noop`.

**Pub/Sub**: `CSubscribe` uses `std::map<std::string, std::list<EventNode>>` with `std::shared_mutex` for thread-safe subscriber management. Event types: DEFAULT, POST_DELAY, NOT_EQUAL_ZERO, EQUAL_ZERO. Separate map for PLC I/O server subscribers.

**Worker initialization** (in `ngx_worker_process_init`): Creates thread pool, starts TimerManager with 5 periodic timers (`timer_500ms`, `timer_1s`, `timer_2s`, `timer_3s`, `timer_5s`) that fire `NotifyTimerSubscriber()`, initializes epoll and send/recycle threads.

## Component Relationships

```
  testapp / toolgplat / testapp2 / testapp3  (client apps)
         |  link libhigplat.so, use network API
         v
      higplat          (client shared library)
         |  TCP socket (port 8777)
         v
       gplat           (server, epoll + thread pool)
         |  calls higplat local API (ReadQ/WriteQ/ReadB/WriteB)
         v
    QBD files          (memory-mapped data files in ./qbdfile/)

  s7ioserver           (PLC bridge daemon)
         |  reads Siemens S7 PLC via Snap7 (libsnap7.so)
         |  writes tags to gPlat Board via higplat network API
         |  receives write-back commands via PLC subscribe
         v
      gplat server  <-->  PLC hardware
```

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
- **doc/** — Documentation: architecture, API reference, contributing guide, message type templates, task plans.

## Key Files

| File | Purpose |
|---|---|
| `gplat/nginx.cxx` | Server entry point (main), QBD file loading, global objects |
| `gplat/ngx_c_slogic.cxx` | Message handler dispatch table (`statusHandler[]`) and all 17 handler implementations |
| `gplat/ngx_c_slogic.h` | `CLogicSocket` class declaration (inherits `CSocekt`) |
| `gplat/ngx_c_socket.cxx` | Epoll initialization, connection management, send queue thread, timeout timers |
| `gplat/ngx_c_socket.h` | `CSocekt` base class, `ngx_connection_s` connection struct, `STRUC_MSG_HEADER` |
| `gplat/ngx_c_socket_request.cxx` | Request parsing state machine (4 states) and response sending |
| `gplat/ngx_c_socket_accept.cxx` | New connection acceptance via `accept4()` |
| `gplat/ngx_c_socket_conn.cxx` | Connection pool: init, get, free, recycling thread |
| `gplat/ngx_process_cycle.cxx` | Master/worker process lifecycle, worker init with timer setup |
| `gplat/CSubscribe.h` | Publish-subscribe engine (header-only, `std::shared_mutex`) |
| `gplat/ngx_c_threadpool.cxx` | Thread pool: create, dispatch, shutdown |
| `higplat/higplat.cpp` | All client API + local QBD operations (~4200 lines) |
| `higplat/qbd.h` | Internal QBD data structures, hash table, constants |
| `include/msg.h` | Binary protocol definition: `MSGID` enum (47 values, 5–51), `MSGHEAD` packed struct |
| `include/higplat.h` | Public API declarations and QBD data structures (Board/Queue/DB headers) |
| `include/timer_manager.h` | `TimerManager`: epoll + timerfd + min-heap, periodic and one-shot timers |
| `include/podstring.h` | `PodString<N>`: stack-allocated, memcpy-safe fixed-capacity string |
| `include/type_code.h` | `TypeCode` enum (Empty through Struct, 13 values) |
| `include/struct_reflect.h` | Compile-time struct reflection: `FieldInfo`, `StructInfo`, `REGISTER_STRUCT` macro |
| `include/user_types.h` | User-defined struct definitions with reflection: `SensorData`, `MotorStatus`, `GPSPosition`, `Vehicle` |
| `include/struct_registry.h` | Global struct name → `StructInfo*` lookup table |
| `s7ioserver/main.cpp` | S7 I/O server entry point |
| `s7ioserver/s7config.h` | PLC configuration structures (`TagConfig`, `PlcConfig`, `AppConfig`) |
| `s7ioserver/s7config.cpp` | INI config parser for s7ioserver |
| `s7ioserver/threadReadPlc.cpp` | PLC read thread: polls S7 PLC, writes changed values to Board |
| `s7ioserver/threadWritePlc.cpp` | PLC write thread: subscribes to Board tags, writes back to PLC |
| `toolgplat/main.cpp` | REPL client with type-aware display, script/config file import |
| `toolgplat/type_handle.h` | Type registry: `TypeInfo`, print functions, S7 type mapping |

## Wire Protocol

Messages use `MSGHEAD` (packed struct, `#pragma pack(1)`) as header followed by a variable-length body (max 16KB per `MAXMSGLEN`). The `MSGID` enum defines 47 operation codes (values 5–51). Adding a new message type requires:
1. Add enum value to `MSGID` in `include/msg.h`
2. Implement handler in `gplat/ngx_c_slogic.cxx`
3. Register handler in `statusHandler[]` array (index must match enum value)
4. Add client-side API in `higplat/higplat.cpp` and declare in `include/higplat.h`

Templates for new handlers and API functions are in `doc/message_handler.template.cpp` and `doc/api_function.template.cpp`.

### MSGID Categories

| Range | Category | Key IDs |
|---|---|---|
| 5–12 | Connection/lifecycle | SUCCEED, FAIL, CONNECT, RECONNECT, DISCONNECT, OPEN, OPENQ, CLOSEQ |
| 13–18 | Queue operations | CLEARQ, ISEMPTYQ, ISFULLQ, READQ, PEEKQ, WRITEQ |
| 19–21 | Board binary R/W | READB, READBSTRING, WRITEB |
| 22–30 | Misc/DB | QDATA, READHEAD, MULREADQ, SETPTRQ, WATCHDOG, SELECTTB, CLEARTB, INSERTTB, REFRESHTB |
| 31–38 | Metadata/management | READTYPE, CREATEITEM, CREATETABLE, DELETEITEM, DELETETABLE, READHEADB, READHEADDB, ACK |
| 39–48 | Extended ops | POPARECORDQ, WRITEBSTRING, WRITETOL1, SUBSCRIBE, CANCELSUBSCRIBE, POST, POSTWAIT, PASSTOSERVER, CLEARB, CLEARDB |
| 49–51 | PLC integration | REGISTERPLCSERVER, WRITEBPLC, WRITEBSTRINGPLC |

### MSGHEAD Fields

```c
int id;              // MSGID enum value
char qname[40];      // queue/board/DB name
char itemname[40];   // item/tag name
int qbdtype, datasize, datatype;
timespec timestamp;
int start, count, recsize, bodysize;
int readptr, writeptr;
unsigned int error;  // error code in response
int subsize, offset;
char ip[16];
int arraysize;       // PLC array write
int eventid, eventarg, timeout;  // subscribe / postwait
```

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

### Field Description Macros

| Macro | Usage | Description |
|---|---|---|
| `FIELD_DESC(TYPE, STRUCT, NAME)` | Scalar field | TYPE is a `TypeCode` name (Int32, Single, Boolean, etc.) |
| `FIELD_DESC_ARRAY(TYPE, STRUCT, NAME, COUNT)` | Scalar array | Fixed-size array of scalars |
| `FIELD_DESC_STRING(STRUCT, NAME)` | `PodString<N>` field | Auto-detects size via `sizeof` |
| `FIELD_DESC_STRING_ARRAY(STRUCT, NAME, COUNT)` | `PodString<N>` array | Array of fixed-capacity strings |
| `FIELD_DESC_STRUCT(STRUCT, NAME, NESTED)` | Nested struct | One layer of nesting |
| `FIELD_DESC_STRUCT_ARRAY(STRUCT, NAME, NESTED, COUNT)` | Nested struct array | Array of nested structs |

### PodString<N>

Stack-allocated, `memcpy`-safe, fixed-capacity string class (`include/podstring.h`). Key properties:
- `standard_layout` + `trivially_copyable` — safe for `memcpy`, `memset`, and Board storage
- `m_data` at offset 0 — object address equals string address
- N = max chars (not including `'\0'`), internal buffer is N+1
- Overflow throws `std::length_error`
- Type aliases: `PodString8`, `PodString16`, `PodString20`, `PodString40`, `PodString64`, `PodString128`

### TypeCode Enum

```
Empty=0, Boolean=1, Char=2, Int16=3, UInt16=4, Int32=5, UInt32=6,
Int64=7, UInt64=8, Single=9, Double=10, String=11, Struct=12
```

### Registered Struct Types (in `user_types.h`)

| Struct | Fields | Notes |
|---|---|---|
| `SensorData` | temperature(Int32), humidity(Int32), pressure(Double), alarm(Boolean), location(PodString<20>) | Scalar + string |
| `MotorStatus` | speed[3](Single), current(Single), error_code(Int32), run_count(UInt32), motor_name[3](PodString<16>) | Float array + string array |
| `GPSPosition` | latitude(Double), longitude(Double) | Inner struct for nesting |
| `Vehicle` | id(Int32), pos(GPSPosition), history[3](GPSPosition), plate(PodString<16>) | Nested struct + nested struct array |

## S7 PLC Integration

The `s7ioserver` bridges Siemens S7 PLCs with gPlat Board storage using the Snap7 library.

### Architecture

- **Read threads** (one per PLC): Poll PLC data blocks at configurable intervals, detect changes via raw byte comparison, write changed values to Board tags via `write_plc_*` API
- **Write thread** (shared): Subscribes to Board tags, receives change notifications via `waitpostdata()`, writes values back to PLC via Snap7
- **Configuration**: INI-style file (`s7ioserver.ini`) with `[general]` section for gPlat connection and per-PLC sections defining tag mappings

### INI Config Format

```ini
[general]
gplat_server = 127.0.0.1
gplat_port = 8777
board_name = BOARD
reconnect_interval = 3000

[PLC_Line1]
ip = 192.168.1.100
rack = 0
slot = 1
poll_interval = 200

# tag = area, db_number, byte_offset[.bit_offset], datatype [, maxlength]
D1     = DB, 1, 2,    DINT
bool1  = DB, 1, 0.1,  BOOL
s7str1 = DB, 1, 126,  STRING, 100
```

### Supported S7 Data Types

| S7 Type | gPlat TypeCode | Size |
|---|---|---|
| BOOL | Boolean | 1 byte (1 bit in PLC) |
| INT | Int16 | 2 bytes |
| WORD | UInt16 | 2 bytes |
| DINT | Int32 | 4 bytes |
| DWORD | UInt32 | 4 bytes |
| REAL | Single | 4 bytes |
| STRING | Char (variable) | 2 + maxlen bytes |

### PLC API Functions

Type-safe write functions with deleted template overloads to prevent implicit conversions:
- `write_plc_bool`, `write_plc_short`, `write_plc_ushort`, `write_plc_int`, `write_plc_uint`, `write_plc_float`, `write_plc_string`
- `registertag` — Register a tag with PLC server subsystem

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
### Database: `CreateTable`, `InsertTable`, `UpdateTable`, `RefreshTable`, `SelectTable`, `ClearTable`, `DeleteTable`, `ClearDB`, `ReadHeadDB`
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

## Data File Tools

```bash
# Create a Board file (size in bytes)
createb <boardname> -s <size>

# Create a Queue file
createq <queuename> -n <record_count> -l <record_size> [-s]
#   -s  enable shift mode (default: normal mode)
```

Data files are stored in `./qbdfile/` relative to the server working directory.

### toolgplat Commands

| Command | Syntax | Description |
|---|---|---|
| `conn` | `conn [ip]` | Connect to gPlat server (default: 127.0.0.1:8777) |
| `create` | `create <tag> <type> [arraysize]` | Create tag (built-in types, registered structs, or custom `name$size`) |
| `create ... script` | `create ... from script <file>` | Batch create tags from script file |
| `create ... config` | `create ... from config <file>` | Create tags from PLC INI config |
| `select` | `select <tag>` | Read and display tag with type-aware formatting |
| `delete` | `delete <tag>` | Delete a tag |
| `clear` | `clear <name>` | Clear board data |
| `types` | `types` | List all built-in and registered struct types |

## Code Conventions

- Comments are a mix of Chinese (Simplified) and English
- Server-side files use `.cxx` extension; library and tool files use `.cpp`
- Commit messages follow the pattern: `YYMMDD-NN: description` (e.g., `260212-01:修改注释`)
- RAII locking via `CLock` wrapper around `pthread_mutex_t`
- User-defined structs must be `trivially_copyable` (enforced by `static_assert` in `REGISTER_STRUCT`)
- All structs use `#pragma pack(push, 8)` for Board storage; wire protocol uses `#pragma pack(push, 1)`
- No formal test framework; `testapp/`, `testapp2/`, `testapp3/` are manual integration tests

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
