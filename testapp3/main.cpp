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
#include "../include/user_types.h"

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

	SensorData sensorData;
	sensorData.alarm = true;
	sensorData.humidity = 80;
	sensorData.pressure = 1013.25;
	sensorData.temperature = 25;
	sensorData.location = "车间1-区域A";

	SensorData sensorarr[3];
	sensorarr[0] = sensorData;
	sensorarr[1] = sensorData;
	sensorarr[2] = sensorData;
	sensorarr[1].location = "车间1-区域B";
	sensorarr[2].location = "车间1-区域C";

	ret = writeb(h, "sensor1", &sensorData, sizeof(sensorData), &err);
	assert(ret);

	MotorStatus motorStatus;
	motorStatus.current = 12.5f;
	motorStatus.error_code = 0;
	motorStatus.run_count = 12345;
	motorStatus.speed[0] = 1000.0f;
	motorStatus.speed[1] = 1500.0f;
	motorStatus.speed[2] = 2000.0f;
	motorStatus.motor_name[0] = "主电机";
	motorStatus.motor_name[1] = "副电机A";
	motorStatus.motor_name[2] = "副电机B";


	ret = writeb(h, "motorstatus", &motorStatus, sizeof(motorStatus), &err);
	assert(ret);

	printf("exit\n");

	return 0;
}