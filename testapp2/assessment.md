# Assessment: testapp2 Windows-to-Linux Portability

**Solution:** `D:\Program\Linux\gPlat\gPlat.sln`  
**Project:** `D:\Program\Linux\gPlat\testapp2\testapp2.vcxproj`  
**Platform Toolset:** Remote_GCC_1_0 (Linux GCC cross-compilation)  
**Current Branch:** `develop` (has pending changes)  
**Build Result:** 44 errors, 3 warnings  

---

## In-Scope Issues (Windows-to-Linux Portability)

### File: `D:\Program\Linux\gPlat\testapp2\main.cpp` (38 errors, 1 warning in-scope)

| # | Line(s) | Issue | Windows API/Type | Recommended Linux Replacement |
|---|---------|-------|------------------|-------------------------------|
| 1 | 35 | `CPtrList` not declared | MFC `CPtrList` | `std::list<std::thread*>` |
| 2 | 36 | `CWinThread*` not declared | MFC `CWinThread` | `std::thread*` |
| 3 | 39 | `TCHAR` not declared | Windows `TCHAR` typedef | `char` |
| 4 | 49, 96 | `LONGLONG` not declared | Windows `LONGLONG` typedef | `long long` |
| 5 | 57 | `ULONGLONG` not declared | Windows `ULONGLONG` typedef | `unsigned long long` |
| 6 | 46, 52, 63, 73, 99 | `ASSERT` not declared | MFC `ASSERT` macro | `assert` (from `<cassert>`) |
| 7 | 51, 98 | `AfxBeginThread` not declared | MFC thread creation | `new std::thread(...)` |
| 8 | 51, 98 | `THREAD_PRIORITY_NORMAL` not declared | Windows thread priority | Remove (not needed with `std::thread`) |
| 9 | 55, 86, 104 | `Sleep(ms)` not declared | Windows `Sleep` API | `std::this_thread::sleep_for(std::chrono::milliseconds(ms))` |
| 10 | 57, 82, 93 | `GetTickCount64()` not declared | Windows timing API | `std::chrono::steady_clock` based helper |
| 11 | 61, 71 | `wsprintf` with `L"..."` wide literals | Windows `wsprintf` API | `sprintf` with narrow strings |
| 12 | 61, 63, 71, 73 | Wide string literals `L"BOARD"` etc. | Windows wide char | Remove `L` prefix, use narrow `char*` literals |
| 13 | 80 | `exitloop` not declared | Missing `extern` declaration | Add `extern bool exitloop;` |
| 14 | 82, 112 | `endl` not qualified | Missing `std::` prefix | Use `std::endl` |
| 15 | 89, 108 | `InterlockedExchangeAdd` not declared | Windows atomic API | Use `std::atomic<long>` with `.load()` |
| 16 | 89, 108 | `threadcount` not declared | Missing `extern` declaration | Add `extern` for `std::atomic<long>` (refactor from `volatile long`) |
| 17 | 53, 100, 115 | `m_ThreadsList.AddHead` / `RemoveAll` | MFC `CPtrList` methods | `std::list::push_front` / `clear` with cleanup |
| 18 | 118 | `_getch()` not declared | Windows `<conio.h>` | `getchar()` or POSIX terminal input |

### File: `D:\Program\Linux\gPlat\testapp2\threadfunction.cpp` (6 errors)

| # | Line(s) | Issue | Windows API/Type | Recommended Linux Replacement |
|---|---------|-------|------------------|-------------------------------|
| 19 | 56 | `BYTE` not declared in `TagBigData` struct | Windows `BYTE` typedef | `unsigned char` |
| 20 | 64, 172 | `UINT` return type not declared | Windows `UINT` typedef | `unsigned int` |
| 21 | 145, 163 | `assert` not declared | Missing include | Add `#include <cassert>` |
| 22 | 157 | `_wtoi` not declared | Windows wide-char-to-int | `atoi` (tagname is `char[]`, not `wchar_t[]`) |
| 23 | 18 | `volatile long threadcount` with `InterlockedIncrement/Decrement` | Windows atomic API | Refactor to `std::atomic<long>` with `++`/`--` operators |
| 24 | 72, 117, 183, 215 | `InterlockedIncrement` / `InterlockedDecrement` | Windows atomic API | `threadcount.fetch_add(1)` / `threadcount.fetch_sub(1)` or `++`/`--` |
| 25 | 98, 111 | `ULONGLONG` not declared | Windows `ULONGLONG` typedef | `unsigned long long` |
| 26 | 98, 111 | `GetTickCount64()` not declared | Windows timing API | `std::chrono::steady_clock` based helper |

---

## Out-of-Scope Issues (Pre-existing, not related to portability)

| # | File | Line | Issue |
|---|------|------|-------|
| 1 | `D:\Program\Linux\gPlat\testapp2\main.cpp` | 118 | Warning: unused variable 'a' [-Wunused-variable] |
| 2 | `D:\Program\Linux\gPlat\testapp2\function.cpp` | 51 | Warning: unused variable 'rowcount' [-Wunused-variable] |
| 3 | `Microsoft.CppBuild.targets` | 548 | Warning: MSB8028 shared intermediate directory between projects |

---

## Summary of Changes Required

### 1. Replace MFC Types & Thread Model
- Remove MFC `CPtrList` → use `std::list<std::thread*>`
- Remove MFC `CWinThread` → use `std::thread`
- Replace `AfxBeginThread(proc, param, priority)` → `new std::thread(proc, param)`
- Thread functions change from `UINT func(void*)` → `void func(void*)` or `unsigned int func(void*)`

### 2. Replace Windows Typedefs
- `TCHAR` → `char`
- `LONGLONG` → `long long`
- `ULONGLONG` → `unsigned long long`
- `BYTE` → `unsigned char`
- `UINT` → `unsigned int`

### 3. Replace Windows APIs
- `Sleep(ms)` → `std::this_thread::sleep_for(std::chrono::milliseconds(ms))`
- `GetTickCount64()` → Helper using `std::chrono::steady_clock`
- `InterlockedExchangeAdd/Increment/Decrement` → `std::atomic<long>` operations
- `wsprintf(buf, L"fmt", ...)` → `sprintf(buf, "fmt", ...)`
- `_getch()` → `getchar()`
- `_wtoi()` → `atoi()`
- `ASSERT()` → `assert()` with `#include <cassert>`

### 4. Fix Declarations & Includes
- Add `extern bool exitloop;` and `extern std::atomic<long> threadcount;` in `main.cpp`
- Add `#include <cassert>` in `threadfunction.cpp`
- Replace `endl` with `std::endl`
- Remove wide string literal `L"..."` prefixes (use narrow `char*` strings throughout)

### 5. Refactor `threadcount`
- Change `volatile long threadcount` to `std::atomic<long> threadcount` in `threadfunction.cpp`
- This enables portable atomic operations across Windows and Linux
