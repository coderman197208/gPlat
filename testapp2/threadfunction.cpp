#include <thread>
#include <string>
#include <cstdio>
#include <atomic>
#include <iostream>
#include <chrono>
#include <string.h>

#include "../include/higplat.h"
#include "../include/user_types.h"

using namespace std;

// 跨平台的毫秒级时间戳函数，替代 Windows GetTickCount64()
inline unsigned long long GetTickCount64()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

thread_local int serverHandle;

std::atomic<long> threadcount(0);

bool exitloop = false;

std::atomic<int> g_failures(0);

#define CHECK(cond, ...)                                  \
	do {                                                  \
		if (!(cond)) {                                    \
			printf("[FAIL] %s:%d ", __FILE__, __LINE__); \
			printf(__VA_ARGS__);                          \
			printf("\n");                                 \
			g_failures++;                                 \
		}                                                 \
	} while (0)

#define LOOPCOUNT   50

unsigned int TestThreadProc1(void* pParam)
{
	int h;
	unsigned int  err;
	bool   ret;
	int i, j;

	++threadcount;

	cout << "thread start: " << (int)(intptr_t)pParam << endl;

	h = connectgplat("127.0.0.1", 8777);
	if (h < 0)
	{
		printf("连接失败\n");
		--threadcount;
		return 0;
	}

	char tagname[100][32];
	for (i = 0; i < 100; i++)
	{
		sprintf(tagname[i], "tagint%02d_%02d", i, (int)(intptr_t)pParam);
	}

	TagBigData tagBigData1;
	tagBigData1.a = 666;
	tagBigData1.b = 666;
	tagBigData1.c = 5.55f;
	ret = writeb(h, "TagBigData1", &tagBigData1, sizeof(TagBigData), &err);
	CHECK(ret, "writeb TagBigData1 failed, error=%u", err);

	string str1(10000, 'A');
	unsigned long long tickcount1 = GetTickCount64();
	for (i = 0; i < LOOPCOUNT; i++)
	{
		for (j = 0; j < 100; j++)
		{
			ret = writeb(h, tagname[j], &i, sizeof(int), &err);
			CHECK(ret, "writeb %s failed, error=%u", tagname[j], err);

			ret = writeb(h, "TagBigData1", &tagBigData1, sizeof(TagBigData), &err);
			CHECK(ret, "writeb TagBigData1 failed, error=%u", err);

			ret = writeb_string(h, "string1", str1.c_str(), &err);
			CHECK(ret, "writeb_string string1 failed, error=%u", err);
		}
	}
	unsigned long long tickcount2 = GetTickCount64();
	printf("TestThreadProc1 used time = %lld\n", tickcount2 - tickcount1);

	TagBigData tagBigData2;
	tagBigData2.b = -1;
	ret = readb(h, "TagBigData1", &tagBigData2, sizeof(TagBigData), &err, nullptr);
	CHECK(ret, "readb TagBigData1 failed, error=%u", err);
	CHECK(tagBigData1.b == tagBigData2.b, "TagBigData1.b mismatch: expected=%lld got=%lld", tagBigData1.b, tagBigData2.b);

	char buffer[10001]{};	//读的时候要多一个字符空间，用于存放字符串结束符
	ret = readb_string(h, "string1", buffer, 10001, &err, nullptr);
	CHECK(ret, "readb_string string1 failed, error=%u", err);
	string str2(buffer);
	CHECK(str1 == str2, "string1 content mismatch");

	disconnectgplat(h);

	--threadcount;

	return 0;
}

void SubscribeEvent(int threadIndex)
{
	bool ret;
	unsigned int err;

	for (int i = 0; i < 100; i++)
	{
		char tagname[32];
		sprintf(tagname, "tagint%02d_%02d", i, threadIndex);
		ret = subscribe(serverHandle, tagname, &err);
		CHECK(ret, "subscribe %s failed, error=%u", tagname, err);
	}
}

void DataChangedHandler(string& eventname, void* pdata, int datasize)
{
	bool ret;
	unsigned int err;
	int newvalue = *(int*)pdata;

	char tagname[32];
	strcpy(tagname, eventname.c_str());
	int subfix = atoi(tagname + 9);
	subfix++;
	sprintf(tagname + 9, "%02d", subfix);

	int oldvalue;
	ret = readb(serverHandle, tagname, &oldvalue, sizeof(int), &err, nullptr);
	CHECK(ret, "readb %s failed, error=%u", tagname, err);
	if (newvalue - oldvalue != 1)
	{
		cout << "验证失败" << endl;
	}
	ret = writeb(serverHandle, tagname, &newvalue, sizeof(int), &err);
	CHECK(ret, "writeb %s failed, error=%u", tagname, err);
}

unsigned int TestThreadProc2(void* pParam)
{
	unsigned int  errorcode;

	++threadcount;

	serverHandle = connectgplat("127.0.0.1", 8777);
	if (serverHandle < 0)
	{
		cout << "连接失败" << endl;
		--threadcount;
		return 0;
	}

	int threadIndex = (int)(intptr_t)pParam;
	SubscribeEvent(threadIndex);

	string eventname = "";
	while (1)
	{
		char pdata[4096];
		int  buffsize = 4096;
		char name[GPLAT_TAGNAME_SIZE];
		waitpostdata(serverHandle, name, sizeof(name), pdata, buffsize, 500, &errorcode);
		eventname = name;
		if (eventname == "WAIT_TIMEOUT")
		{
			//可以在这里执行周期类任务、控制线程退出等等
			if (exitloop)
				break;
			continue;
		}

		DataChangedHandler(eventname, pdata, buffsize);
	}

	disconnectgplat(serverHandle);

	--threadcount;

	return 0;
}
