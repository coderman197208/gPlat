#include <thread>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <iostream>
#include <string.h>

#include "../include/higplat.h"
#include "../include/user_types.h"

extern std::atomic<bool> g_running;  // 控制线程运行的标志

void threadFunction1() {
	int conngplat;
	conngplat = connectgplat("127.0.0.1", 8777);
	bool ret{ false };
	unsigned int error;

	subscribe(conngplat, "WATCHDOG", &error);
	subscribe(conngplat, "SPRAY_STRING_TO_L1", &error);
	subscribe(conngplat, "MotorStatusQueue", &error);

	int a = 0;
	std::string eventname;
	while (g_running) {  // 检查全局运行标志 {
		char value[4096] = { 0 };
		waitpostdata(conngplat, eventname, value, 4096, -1, &error); // 等待数据到达

		if (error != 0) {
			printf("waitpostdata failed, eventname=%s, error=%d\n", eventname.c_str(), error);
			continue;
		}

		printf("eventname=%s, error=%d\n", eventname.c_str(), error);

		if (eventname == "WATCHDOG") {
			int b = read_value<int>(value);
			printf("WATCHDOG=%d\n", b);
		}
		else if (eventname == "SPRAY_STRING_TO_L1") {
			char *s = read_value<char*>(value);
			printf("SPRAY_STRING_TO_L1=%s\n", s);
		}
		else if (eventname == "MotorStatusQueue") {
			MotorStatus motorStatus = read_value<MotorStatus>(value);
			printf("motorStatus.motor_name[0]=%s\n", motorStatus.motor_name[0].c_str());
		}
	}
	printf("work thread exit\n");
}

void threadFunction2() {
	int conngplat;
	conngplat = connectgplat("127.0.0.1", 8777);
	bool ret{ false };
	unsigned int error;
}

void threadFunction3() {

}