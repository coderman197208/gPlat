#include <cstdio>
#include <chrono>
#include <thread>       //std::this_thread::sleep_for   std::chrono::milliseconds
#include <atomic>
#include <iostream>
#include <list>
#include <string.h>

#include "../include/higplat.h"
#include "../include/qbdtype.h"

// 外部变量声明（定义在 threadfunction.cpp)
extern std::atomic<long> threadcount;
extern bool exitloop;
extern std::atomic<int> g_failures;

// 前向声明线程函数
unsigned int TestThreadProc1(void* pParam);
unsigned int TestThreadProc2(void* pParam);

// 跨平台的毫秒级时间戳函数，替代 Windows GetTickCount64()
inline unsigned long long GetTickCount64()
{
    using namespace std::chrono;
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 跨平台的线程启动辅助函数，替代 MFC AfxBeginThread
std::thread* BeginThread(unsigned int (*proc)(void*), void* param)
{
    return new std::thread([proc, param]() {
        proc(param);
    });
}

#define CHECK(cond, ...)                                  \
	do {                                                  \
		if (!(cond)) {                                    \
			printf("[FAIL] %s:%d ", __FILE__, __LINE__); \
			printf(__VA_ARGS__);                          \
			printf("\n");                                 \
			g_failures++;                                 \
		}                                                 \
	} while (0)

int main()
{
    int h;
    unsigned int  err;
    bool   ret = 0;

    h = connectgplat("127.0.0.1", 8777);
    if (h < 0)
    {
		printf("连接失败，error=%d\n", h);
        return 0;
    }
    printf("连接成功\n");

	
	std::list<std::thread*> m_ThreadsList;
	std::thread* m_pThread;

	int value = -1;
	char tagname[100][32];
	for (int i = 0; i < 11; i++)
	{
		for (int j = 0; j < 100; j++)
		{
			sprintf(tagname[j], "tagint%02d_%02d", j, i);
			ret = writeb(h, tagname[j], &value, sizeof(int), &err);
			CHECK(ret, "writeb %s failed, error=%u", tagname[j], err);
		}
	}
	for (long long i = 0; i < 10; i++)
	{
		m_pThread = BeginThread(TestThreadProc2, (void*)i);
		CHECK(m_pThread != NULL, "BeginThread TestThreadProc2 failed");
		m_ThreadsList.push_front(m_pThread);
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(500));

	unsigned long long tickcount = GetTickCount64();
	value = 0;
	for (int j = 0; j < 100; j++)
	{
		sprintf(tagname[j], "tagint%02d_00", j);
		ret = writeb(h, tagname[j], &value, sizeof(int), &err);
		CHECK(ret, "writeb %s failed, error=%u", tagname[j], err);
	}

	while (true)
	{
		int j;
		for (j = 0; j < 100; j++)
		{
			sprintf(tagname[j], "tagint%02d_10", j);
			ret = readb(h, tagname[j], &value, sizeof(int), &err, nullptr);
			CHECK(ret, "readb %s failed, error=%u", tagname[j], err);

			if (value != 0) break;
		}

		if (j == 100) break;
	}
	exitloop = true;

	std::cout << "1阶段耗时：" << GetTickCount64() - tickcount << std::endl;

	while (true)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(10));

		if (threadcount.load() == 0) break;
	}

	tickcount = GetTickCount64();

	for (long long i = 0; i < 10; i++)
	{
		m_pThread = BeginThread(TestThreadProc1, (void*)i);
		CHECK(m_pThread != NULL, "BeginThread TestThreadProc1 failed");
		m_ThreadsList.push_front(m_pThread);
	}

	while (true)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(10));

		if (threadcount.load() == 0) break;
	}

	std::cout << "2阶段耗时：" << GetTickCount64() - tickcount << std::endl;

	for (auto* pThread : m_ThreadsList)
	{
		if (pThread->joinable())
			pThread->join();
		delete pThread;
	}
	m_ThreadsList.clear();

	disconnectgplat(h);

	if (g_failures == 0)
		printf("ALL PASSED\n");
	else
		printf("%d FAILURE(S)\n", g_failures.load());

	return g_failures == 0 ? 0 : 1;
}
