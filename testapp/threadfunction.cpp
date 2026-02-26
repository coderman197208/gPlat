#include <thread>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <iostream>
#include <string.h>

#include "../include/higplat.h"

extern std::atomic<bool> g_running;  // 控制线程运行的标志

void threadFunction1() {
	int conngplat;
	conngplat = connectgplat("127.0.0.1", 8777);
	bool ret{ false };
	unsigned int error;
	subscribe(conngplat, "int1", &error);
	subscribedelaypost(conngplat, "int1", "int1_delay1000", 1000, &error); // 延时订阅
	subscribe(conngplat, "string1", &error);
	//   subscribe(conngplat, "timer_500ms", &error);
	//   subscribe(conngplat, "timer_1s", &error);
	//   subscribe(conngplat, "timer_2s", &error);
	//   subscribe(conngplat, "timer_3s", &error);
	//   subscribe(conngplat, "timer_5s", &error);
	subscribe(conngplat, "D1", &error);
	subscribe(conngplat, "bool1", &error);

	int a = 0;
	std::string eventname;
	while (g_running) {  // 检查全局运行标志 {
		char value[4096] = { 0 };
		waitpostdata(conngplat, eventname, value, 4096, -1, &error); // 等待数据到达

		printf("eventname=%s, error=%d\n", eventname.c_str(), error);

		if (eventname == "bool1") {
			bool a = *((bool*)value);
			printf("bool1=%d\n", a);
			write_plc_bool(conngplat, "bool1", !a, &error); // 写数据
		}
		if (eventname == "D1") {
			int a = *((int*)value);
			printf("D1=%d\n", a);
			write_plc_int(conngplat, "D2", a, &error); // 写数据
		}
		if (eventname == "int1") {
			int a = *((int*)value);
			printf("a=%d\n", a);
			ret = readb(conngplat, "int1", &a, sizeof(a), &error); // 接收数据
			if (!ret) {
				printf("readb failed, error=%d\n", error);
			}
			printf("readb a=%d error=%d\n", a, error);
		}
		else if (eventname == "int1_delay1000") {
			a = *((int*)value);
			printf("int1_delay1000, a=%d\n", a);
			ret = readb(conngplat, "int1", &a, sizeof(a), &error); // 接收数据
			if (!ret) {
				printf("int1_delay1000, readb failed, error=%d\n", error);
			}
			printf("int1_delay1000, readb a=%d error=%d\n", a, error);
		}
		else if (eventname == "int2") {
			ret = readb(conngplat, "int2", &a, sizeof(a), &error); // 接收数据
			if (!ret) {
				printf("readb failed, error=%d\n", error);
			}
		}
		else if (eventname == "string1") {
			std::string str2;
			printf("str2=%s\n", value);
			ret = readb_string2(conngplat, "string1", str2, &error); // 接收数据
			if (ret) {
				printf("readb_string2 str2=%s error=%d\n", str2.c_str(), error);
			}
			else {
				printf("readb_string2 failed, error=%d\n", error);
			}
		}
	}
	printf("work thread exit\n");
}

void threadFunction2() {
	int conngplat;
	conngplat = connectgplat("127.0.0.1", 8777);
	bool ret{ false };
	unsigned int error;

	int a = 0;
	std::string eventname;
	while (g_running) {  // 检查全局运行标志 {
		a++;
		ret = writeb(conngplat, "int1", &a, sizeof(a), &error); // 接收数据;
        if (!ret) {
            printf("writeb failed, error=%d\n", error);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));
	}

	printf("work thread exit\n");
}

void threadFunction3() {
	int conngplat;
	conngplat = connectgplat("127.0.0.1", 8777);
	bool ret{ false };
	unsigned int error;

	int a = 0;
	std::string eventname;
	while (g_running) {
		std::string suffix = std::to_string(a++);
		char str1[200] = "hello world, gyb 1234567890 loop=";
		strcat(str1, suffix.c_str()); // 将数字转换为字符串并复制到 str1
		ret = writeb_string(conngplat, "string1", str1, &error); // 发送数据
		ret = write_plc_string(conngplat, "s7str1", str1, &error); // 发送数据

		//char str2[1000] = { 0 };
		//ret = readb_string(conngplat, "string1", str2, sizeof(str2), &error); // 接收数据
		//if (ret) {
		//    printf("readb_string str2=%s error=%d\n", str2, error);
		//}
		//else {
		//    printf("writeb_string failed, error=%d\n", error);
		//}

		//std::string str2;
		//ret = readb_string2(conngplat, "string1", str2, &error); // 接收数据
		//if (ret) {
		//    printf("readb_string2 str2=%s error=%d\n", str2.c_str(), error);
		//}
		//else {
		//    printf("readb_string2 failed, error=%d\n", error);
		//}

		std::this_thread::sleep_for(std::chrono::milliseconds(1000));
	}

	printf("work thread exit\n");
}