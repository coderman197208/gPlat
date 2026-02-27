# testapp2 Windows-to-Linux Code Migration Tasks

## Overview

This document tracks the Windows-to-Linux migration for the testapp2 project. The migration involves replacing 26 Windows-specific APIs and types with cross-platform C++ standard library equivalents across 2 source files.

**Progress**: 3/3 tasks complete (100%) ![0%](https://progress-bar.xyz/100)

---

## Tasks

### [✓] TASK-001: Fix threadfunction.cpp - types, headers, and cross-platform implementations *(Completed: 2026-02-27 11:01)*
**References**: Plan §阶段2/任务1.1, Plan §阶段2/任务1.2

- [✓] (1) Add `#include <cassert>` header to threadfunction.cpp
- [✓] (2) Replace global `volatile long threadcount` with `std::atomic<long> threadcount(0)` (Line 18)
- [✓] (3) Replace all `InterlockedIncrement(&threadcount)` calls with `++threadcount` or `threadcount.fetch_add(1)` (Lines 72, 183)
- [✓] (4) Replace all `InterlockedDecrement(&threadcount)` calls with `--threadcount` or `threadcount.fetch_sub(1)` (Lines 117, 215)
- [✓] (5) Replace `UINT` return types with `unsigned int` for TestThreadProc1 and TestThreadProc2 functions (Lines 64, 172)
- [✓] (6) Replace `BYTE data[4096]` with `unsigned char data[4096]` in TagBigData struct (Line 56)
- [✓] (7) Replace `ULONGLONG` with `unsigned long long` for tickcount variables (Lines 98, 111)
- [✓] (8) Replace `_wtoi(tagname + 9)` with `atoi(tagname + 9)` (Line 157)
- [✓] (9) Add cross-platform GetTickCount64() helper function using `std::chrono::steady_clock` per Plan §任务1.2
- [✓] (10) File compiles without Windows-specific type errors (**Verify**)
- [⊘] (11) Commit changes with message: "TASK-001: Migrate threadfunction.cpp to cross-platform types and APIs"

---

### [✓] TASK-002: Fix main.cpp - replace MFC/Windows APIs with standard C++ equivalents *(Completed: 2026-02-27 11:01)*
**References**: Plan §阶段2/任务2.1, Plan §阶段2/任务2.2

- [✓] (1) Add required headers: `<cassert>`, `<list>`, `<chrono>` to main.cpp
- [✓] (2) Add extern declarations for `threadcount` and `exitloop` variables
- [✓] (3) Add forward declarations for TestThreadProc1 and TestThreadProc2 functions
- [✓] (4) Add cross-platform GetTickCount64() and BeginThread() helper functions per Plan §任务2.1
- [✓] (5) Replace `CPtrList m_ThreadsList` with `std::list<std::thread*> m_ThreadsList` (Lines 35-36)
- [✓] (6) Replace `CWinThread* m_pThread` with `std::thread* m_pThread` (Line 36)
- [✓] (7) Replace `TCHAR tagname[100][32]` with `char tagname[100][32]` (Line 39)
- [✓] (8) Replace all `ASSERT` calls with `assert` (Lines 46, 52, 63, 73, 99)
- [✓] (9) Replace `LONGLONG` loop variables with `long long` (Lines 49, 96)
- [✓] (10) Replace all `AfxBeginThread` calls with `BeginThread` helper function (Lines 51, 98)
- [✓] (11) Replace `m_ThreadsList.AddHead` with `m_ThreadsList.push_front` (Lines 53, 100)
- [✓] (12) Replace `Sleep(500)` with `std::this_thread::sleep_for(std::chrono::milliseconds(500))` (Line 55)
- [✓] (13) Replace `Sleep(10)` with `std::this_thread::sleep_for(std::chrono::milliseconds(10))` (Lines 86, 104)
- [✓] (14) Replace `ULONGLONG tickcount` with `unsigned long long tickcount` (Line 57)
- [✓] (15) Replace `wsprintf` with `sprintf` and remove `L` prefixes from string literals (Lines 61, 71)
- [✓] (16) Fix `endl` to `std::endl` (Lines 82, 112)
- [✓] (17) Replace `InterlockedExchangeAdd(&threadcount, 0)` with `threadcount.load()` (Lines 89, 108)
- [✓] (18) Replace `m_ThreadsList.RemoveAll()` with proper thread cleanup: join all threads, delete pointers, then clear list (Line 115)
- [✓] (19) Replace `_getch()` with `getchar()` (Line 118)
- [✓] (20) File compiles without MFC/Windows API errors (**Verify**)
- [⊘] (21) Commit changes with message: "TASK-002: Migrate main.cpp to cross-platform C++ standard library"

---

### [✓] TASK-003: Rebuild and verify all Windows-specific errors resolved *(Completed: 2026-02-27 03:02)*
**References**: Plan §阶段3/任务3.1

- [✓] (1) Rebuild testapp2 project using `cppupgrade_rebuild_and_get_issues` tool
- [✓] (2) All 26 in-scope errors resolved, 0 build errors remain (**Verify**)
- [✓] (3) Only 3 out-of-scope warnings remain (unused variables) as expected (**Verify**)

---

