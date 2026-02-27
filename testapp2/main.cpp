#include <cstdio>
#include <chrono>
#include <thread>       //std::this_thread::sleep_for   std::chrono::milliseconds
#include <atomic>
#include <signal.h>
#include <iostream>
#include <cassert>
#include <list>
#include <string.h>

#include "../include/higplat.h"
#include "../include/qbdtype.h"

std::atomic<bool> g_running(true);  // 控制线程运行的标志

// 外部变量声明（定义在 threadfunction.cpp)
extern std::atomic<long> threadcount;
extern bool exitloop;

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

void threadFunction1();
void threadFunction2();
void threadFunction3();

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
			assert(ret);
		}
	}
	for (long long i = 0; i < 10; i++)
	{
		m_pThread = BeginThread(TestThreadProc2, (void*)i);
		assert(m_pThread != NULL);
		m_ThreadsList.push_front(m_pThread);
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(500));

	unsigned long long tickcount = GetTickCount64();
	value = 0;
	for (int j = 0; j < 100; j++)
	{
		sprintf(tagname[j], "tagint%02d_00", j);
		ret = writeb(h, tagname[j], &value, sizeof(int), &err);
		assert(ret);
	}

	while (true)
	{
		int j;
		for (j = 0; j < 100; j++)
		{
			sprintf(tagname[j], "tagint%02d_10", j);
			ret = readb(h, tagname[j], &value, sizeof(int), &err);
			assert(ret);

			if (value != 0) break;
		}

		if (j == 100) break;
	}
	exitloop = true;

	std::cout << "1阶段耗时：" << GetTickCount64() - tickcount << std::endl;

	while (true)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(10));

		long a;
		a = threadcount.load();

		if (a == 0) break;
	}

	tickcount = GetTickCount64();

	for (long long i = 0; i < 10; i++)
	{
		m_pThread = BeginThread(TestThreadProc1, (void*)i);
		assert(m_pThread != NULL);
		m_ThreadsList.push_front(m_pThread);
	}

	while (true)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(10));

		long a;
		a = threadcount.load();

		if (a == 0) break;
	}

	std::cout << "2阶段耗时：" << GetTickCount64() - tickcount << std::endl;

	for (auto* pThread : m_ThreadsList)
	{
		if (pThread->joinable())
			pThread->join();
		delete pThread;
	}
	m_ThreadsList.clear();

	printf("exit\n");
	//getchar();

	return 0;
}