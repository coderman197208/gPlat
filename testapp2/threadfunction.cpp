#include <thread>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <iostream>
#include <cassert>
#include <chrono>
#include <string.h>

#include "../include/higplat.h"

extern std::atomic<bool> g_running;  // 控制线程运行的标志

using namespace std;

// 跨平台的毫秒级时间戳函数，替代 Windows GetTickCount64()
inline unsigned long long GetTickCount64()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

#define OFFSET(structure, member) ((int64_t)&((structure*)0)->member) // 64位系统

thread_local int serverHandle;

std::atomic<long> threadcount(0);

bool exitloop = false;

#define LOOPCOUNT   50

struct DemoTag
{
	int b;
	int a;

	char order_no[16];				    //合同号
	char melt_no[16];					//炉号
	char lot_no[8];						//试批号
	char roll_no[8];					//轧批号
	char comment[20];					//备注
};

struct TubeInfo
{
	//[MarshalAs(UnmanagedType.ByValTStr, SizeConst = 8)]
	char tube_no[8];                  //管号
	//[MarshalAs(UnmanagedType.ByValTStr, SizeConst = 16)]
	char order_no[16];				    //合同号
	//[MarshalAs(UnmanagedType.ByValTStr, SizeConst = 16)]
	char melt_no[16];					//炉号
	//[MarshalAs(UnmanagedType.ByValTStr, SizeConst = 8)]
	char lot_no[8];					//试批号
	//[MarshalAs(UnmanagedType.ByValTStr, SizeConst = 8)]
	char roll_no[8];					//轧批号
	//[MarshalAs(UnmanagedType.ByValTStr, SizeConst = 20)]
	char comment[20];					//备注
};

struct TagBigData
{
	long long a;
	unsigned char data[3992];
	char str1[3992];
	char str2[3992];
	char str3[3992];
	long long b;
	float c;
};

unsigned int TestThreadProc1(void* pParam)
{
	int h;
	unsigned int  err;
	bool   ret;
	string input;
	int i, j;

	//cout << "sizeof(TagBigData)=" << sizeof(TagBigData) << endl;

	++threadcount;

	//cout << "thread start: " << (int)pParam << endl;

	h = connectgplat("127.0.0.1", 8777);
	if (h < 0)
	{
		printf("连接失败\n");
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
	assert(ret);

	string str1(10000, 'A');
	unsigned long long tickcount1 = GetTickCount64();
	for (i = 0; i < LOOPCOUNT; i++)
	{
		for (j = 0; j < 100; j++)
		{
			ret = writeb(h, tagname[j], &i, sizeof(int), &err);
			//ret = writeb(h, "tagint00_00", &i, sizeof(int), &err);
			assert(ret);

			ret = writeb(h, "TagBigData1", &tagBigData1, sizeof(TagBigData), &err);
			assert(ret);

			ret = writeb_string(h, "string1", str1.c_str(), &err);
			assert(ret);
		}
	}
	unsigned long long tickcount2 = GetTickCount64();
	printf("TestThreadProc1 used time = %lld\n", tickcount2 - tickcount1);

	TagBigData tagBigData2;
	tagBigData2.b = -1;
	ret = readb(h, "TagBigData1", &tagBigData2, sizeof(TagBigData), &err);
	assert(ret);
	assert(tagBigData1.b == tagBigData2.b);

	char buffer[10001]{};	//读的时候要多一个字符空间，用于存放字符串结束符
	ret = readb_string(h, "string1", buffer, 10001, &err);
	assert(ret);
	string str2(buffer);
	assert(str1 == str2);

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
		assert(ret);
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
	ret = readb(serverHandle, tagname, &oldvalue, sizeof(int), &err);
	assert(ret);
	if (newvalue - oldvalue != 1)
	{
		cout << "验证失败" << endl;
	}
	ret = writeb(serverHandle, tagname, &newvalue, sizeof(int), &err);
	assert(ret);
}

unsigned int TestThreadProc2(void* pParam)
{
	string input;
	unsigned int  errorcode;

	++threadcount;

	serverHandle = connectgplat("127.0.0.1", 8777);
	if (serverHandle < 0)
	{
		cout << "连接失败" << endl;
		return 0;
	}

	int threadIndex = (int)(intptr_t)pParam;
	SubscribeEvent(threadIndex);

	string eventname = "";
	while (1)
	{
		bool ret;
		char pdata[4096];
		int  buffsize = 4096;
		ret = waitpostdata(serverHandle, eventname, pdata, buffsize, 500, &errorcode);
		if (eventname == "WAIT_TIMEOUT")
		{
			//可以在这里执行周期类任务、控制线程退出等等
			if (exitloop)
				break;
		}

		DataChangedHandler(eventname, pdata, buffsize);
	}

	disconnectgplat(serverHandle);

	--threadcount;

	return 0;
}