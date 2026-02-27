# Plan: testapp2 Windows-to-Linux 代码移植

**项目:** `D:\Program\Linux\gPlat\testapp2\testapp2.vcxproj`  
**当前分支:** `develop` (有pending changes)  
**构建错误:** 44 errors, 3 warnings  
**修复范围:** 26个Windows专用API/类型替换为跨平台实现

---

## 执行策略

### 阶段概述

1. **准备阶段**: 创建新分支，确保原始代码安全
2. **修复阶段**: 按依赖顺序修复所有Windows专用代码
3. **验证阶段**: 编译验证，确保没有引入新错误

### 修复顺序原则

修复顺序基于**依赖关系**和**影响范围**：
1. 先修复类型定义（底层）
2. 再修复全局变量声明（中层）
3. 最后修复API调用（顶层）

这样可以避免级联错误，每一步修复都建立在前一步的基础上。

---

## 详细修复方案

### 阶段1: 类型定义与头文件

#### 任务1.1: 添加必要的头文件和类型替换

**文件:** `D:\Program\Linux\gPlat\testapp2\threadfunction.cpp`

**修改内容:**

1. **添加头文件**
   ```cpp
   #include <cassert>  // 添加此行，支持 assert()
   ```

2. **修改全局变量 threadcount**
   - **原代码 (Line 18):**
     ```cpp
     volatile long threadcount = 0;
     ```
   - **新代码:**
     ```cpp
     std::atomic<long> threadcount(0);
     ```
   - **原因:** 使用 `std::atomic` 替代 `volatile` + Windows Interlocked* APIs，提供跨平台的原子操作

3. **修改所有 Interlocked* 调用**
   - **InterlockedIncrement (Line 72, 183):**
     - 原: `InterlockedIncrement(&threadcount);`
     - 新: `threadcount.fetch_add(1);` 或 `++threadcount;`
   
   - **InterlockedDecrement (Line 117, 215):**
     - 原: `InterlockedDecrement(&threadcount);`
     - 新: `threadcount.fetch_sub(1);` 或 `--threadcount;`

4. **修改函数返回类型**
   - **Line 64, 172 的 UINT:**
     ```cpp
     // 原:
     UINT TestThreadProc1(void* pParam)
     UINT TestThreadProc2(void* pParam)
     
     // 新:
     unsigned int TestThreadProc1(void* pParam)
     unsigned int TestThreadProc2(void* pParam)
     ```

5. **修改 struct TagBigData (Line 56)**
   ```cpp
   // 原:
   BYTE data[4096];
   
   // 新:
   unsigned char data[4096];
   ```

6. **修改 ULONGLONG 类型 (Line 98, 111)**
   ```cpp
   // 原:
   ULONGLONG tickcount1 = GetTickCount64();
   ULONGLONG tickcount2 = GetTickCount64();
   
   // 新:
   unsigned long long tickcount1 = GetTickCount64();
   unsigned long long tickcount2 = GetTickCount64();
   ```

7. **修改 _wtoi 调用 (Line 157)**
   ```cpp
   // 原:
   int subfix = _wtoi(tagname + 9);
   
   // 新:
   int subfix = atoi(tagname + 9);
   ```
   - **原因:** `tagname` 是 `char[]` 数组（不是 `wchar_t[]`），应该用 `atoi` 而不是 `_wtoi`

---

#### 任务1.2: 创建跨平台的 GetTickCount64 辅助函数

由于 `GetTickCount64()` 在多个地方被调用，创建一个辅助函数统一实现。

**文件:** `D:\Program\Linux\gPlat\testapp2\threadfunction.cpp`

**在文件顶部添加 (在 #include 之后，全局变量之前):**

```cpp
// 跨平台的毫秒级时间戳函数
inline unsigned long long GetTickCount64()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
```

**注意:** 需要确保已包含 `<chrono>` 头文件（当前文件已通过其他头文件间接包含）

---

### 阶段2: main.cpp 的修复

#### 任务2.1: 添加头文件和全局变量声明

**文件:** `D:\Program\Linux\gPlat\testapp2\main.cpp`

**在 #include 部分添加:**
```cpp
#include <cassert>      // 支持 assert
#include <list>         // 替代 MFC CPtrList
#include <chrono>       // 时间功能
```

**在函数声明前添加外部变量声明:**
```cpp
// 在 main() 函数之前添加
extern std::atomic<long> threadcount;  // 声明在 threadfunction.cpp 中定义的变量
extern bool exitloop;                  // 声明在 threadfunction.cpp 中定义的变量

// 前向声明线程函数
unsigned int TestThreadProc1(void* pParam);
unsigned int TestThreadProc2(void* pParam);
```

**添加跨平台辅助函数:**
```cpp
// GetTickCount64 的跨平台实现
inline unsigned long long GetTickCount64()
{
    using namespace std::chrono;
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 跨平台的线程启动辅助函数
std::thread* BeginThread(unsigned int (*proc)(void*), void* param)
{
    return new std::thread([proc, param]() {
        proc(param);
    });
}
```

---

#### 任务2.2: 修改 main() 函数中的类型和变量

**文件:** `D:\Program\Linux\gPlat\testapp2\main.cpp`

**修改内容 (按行号顺序):**

1. **Line 35-36: 替换 MFC 线程类型**
   ```cpp
   // 原:
   CPtrList m_ThreadsList;
   CWinThread* m_pThread;
   
   // 新:
   std::list<std::thread*> m_ThreadsList;
   std::thread* m_pThread;
   ```

2. **Line 39: 替换 TCHAR**
   ```cpp
   // 原:
   TCHAR tagname[100][32];
   
   // 新:
   char tagname[100][32];
   ```

3. **Line 44: sprintf 正常，无需修改**
   - `sprintf(tagname[j], "TagInt%02d_%02d", j, i);` ✅ 正确

4. **Line 46, 52, 63, 73, 99: 替换 ASSERT 为 assert**
   ```cpp
   // 原:
   ASSERT(ret);
   ASSERT(m_pThread != NULL);
   
   // 新:
   assert(ret);
   assert(m_pThread != NULL);
   ```

5. **Line 49, 96: 替换 LONGLONG**
   ```cpp
   // 原:
   for (LONGLONG i = 0; i < 10; i++)
   
   // 新:
   for (long long i = 0; i < 10; i++)
   ```

6. **Line 51, 98: 替换 AfxBeginThread**
   ```cpp
   // 原:
   m_pThread = AfxBeginThread(TestThreadProc2, (void*)i, THREAD_PRIORITY_NORMAL);
   m_pThread = AfxBeginThread(TestThreadProc1, (void*)i, THREAD_PRIORITY_NORMAL);
   
   // 新:
   m_pThread = BeginThread(TestThreadProc2, (void*)i);
   m_pThread = BeginThread(TestThreadProc1, (void*)i);
   ```

7. **Line 53, 100: 替换 AddHead**
   ```cpp
   // 原:
   m_ThreadsList.AddHead((void*)m_pThread);
   
   // 新:
   m_ThreadsList.push_front(m_pThread);
   ```

8. **Line 55: 替换 Sleep**
   ```cpp
   // 原:
   Sleep(500);
   
   // 新:
   std::this_thread::sleep_for(std::chrono::milliseconds(500));
   ```

9. **Line 57: 替换 ULONGLONG**
   ```cpp
   // 原:
   ULONGLONG tickcount = GetTickCount64();
   
   // 新:
   unsigned long long tickcount = GetTickCount64();
   ```

10. **Line 61, 71: 去掉 wsprintf 和 L 前缀**
    ```cpp
    // 原:
    wsprintf(tagname[j], L"TagInt%02d_00", j);
    ret = WriteB(h, L"BOARD", tagname[j], &value, sizeof(int), 0, 0, &err);
    
    wsprintf(tagname[j], L"TagInt%02d_10", j);
    ret = ReadB(h, L"BOARD", tagname[j], &value, sizeof(int), &err);
    
    // 新:
    sprintf(tagname[j], "TagInt%02d_00", j);
    ret = WriteB(h, "BOARD", tagname[j], &value, sizeof(int), 0, 0, &err);
    
    sprintf(tagname[j], "TagInt%02d_10", j);
    ret = ReadB(h, "BOARD", tagname[j], &value, sizeof(int), &err);
    ```

11. **Line 82, 112: 修正 endl**
    ```cpp
    // 原:
    std::cout << "1阶段耗时：" << GetTickCount64() - tickcount << endl;
    std::cout << "2阶段耗时：" << GetTickCount64() - tickcount << endl;
    
    // 新:
    std::cout << "1阶段耗时：" << GetTickCount64() - tickcount << std::endl;
    std::cout << "2阶段耗时：" << GetTickCount64() - tickcount << std::endl;
    ```

12. **Line 86, 104: 替换 Sleep**
    ```cpp
    // 原:
    Sleep(10);
    
    // 新:
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ```

13. **Line 89, 108: 替换 InterlockedExchangeAdd**
    ```cpp
    // 原:
    a = InterlockedExchangeAdd(&threadcount, 0);
    
    // 新:
    a = threadcount.load();
    ```

14. **Line 115: 替换 RemoveAll 并清理线程**
    ```cpp
    // 原:
    m_ThreadsList.RemoveAll();
    
    // 新:
    for (auto* pThread : m_ThreadsList)
    {
        if (pThread->joinable())
            pThread->join();
        delete pThread;
    }
    m_ThreadsList.clear();
    ```

15. **Line 118: 替换 _getch**
    ```cpp
    // 原:
    int a = _getch();
    
    // 新:
    int a = getchar();
    ```

---

### 阶段3: 验证与清理

#### 任务3.1: 重新编译并验证错误修复

**执行:** 使用 `cppupgrade_rebuild_and_get_issues` 工具重新编译

**预期结果:**
- 所有26个in-scope错误应全部修复
- 仅保留3个out-of-scope警告（未使用变量）

#### 任务3.2: (可选) 修复 out-of-scope 警告

如果需要清理所有警告：

1. **D:\Program\Linux\gPlat\testapp2\main.cpp Line 118**
   ```cpp
   // 原:
   int a = getchar();
   
   // 新:
   getchar();  // 移除未使用的变量
   ```

2. **D:\Program\Linux\gPlat\testapp2\function.cpp Line 51**
   ```cpp
   // 原:
   int rowcount = datasize / sizeof(TubeInfo);
   
   // 新:
   // int rowcount = datasize / sizeof(TubeInfo);  // 注释掉未使用的变量
   ```

---

## 风险评估与注意事项

### 低风险
- ✅ 类型替换（`TCHAR` → `char` 等）：直接等价替换
- ✅ `assert` 替换 `ASSERT`：功能完全相同
- ✅ `std::atomic` 替换 `volatile` + Interlocked*：标准C++，更安全

### 中等风险
- ⚠️ **线程模型变更**: `AfxBeginThread` → `std::thread`
  - **注意:** `std::thread` 需要手动 `join()` 或 `detach()`，否则析构会导致 `std::terminate()`
  - **已处理:** 在 `m_ThreadsList.clear()` 前添加 `join()` 循环

- ⚠️ **GetTickCount64 精度**: Windows版本返回系统启动后的毫秒数，`std::chrono::steady_clock` 同样保证单调递增，精度可能更高
  - **影响:** 计时结果可能略有差异，但逻辑无影响

### 需要测试的点
1. **线程同步**: `std::atomic<long>` 与原 `InterlockedIncrement/Decrement` 行为一致性
2. **时间测量**: 两个阶段的耗时计算是否正常
3. **字符串处理**: 去掉 `L` 前缀后，确保 `WriteB/ReadB` API 期望 `char*` 而非 `wchar_t*`

---

## 修复后的代码特性

### 跨平台兼容性
- ✅ 使用标准C++11/14/17特性（`std::thread`, `std::atomic`, `std::chrono`）
- ✅ 移除所有Windows专用API和MFC依赖
- ✅ 保持原有业务逻辑不变

### 可维护性
- ✅ 代码更现代化，符合C++标准库最佳实践
- ✅ 线程安全由 `std::atomic` 保证，无需手动内存屏障
- ✅ 类型更明确（`unsigned long long` 比 `ULONGLONG` 更清晰）

---

## 总结

**修复范围:** 2个源文件  
**修改行数:** 约40-50处  
**预估工作量:** 30-45分钟  
**兼容性:** 完全跨平台（Windows/Linux/macOS）  

**下一步:** 生成tasks.md并开始执行修复
