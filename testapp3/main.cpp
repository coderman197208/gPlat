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
	ret = writeb(h, "sensor1", &sensorData, sizeof(sensorData), &err);
	assert(ret);

	SensorData sensorarr[3];
	sensorarr[0] = sensorData;
	sensorarr[1] = sensorData;
	sensorarr[2] = sensorData;
	sensorarr[1].location = "车间1-区域B";
	sensorarr[2].location = "车间1-区域C";

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

	Vehicle vehicle;
	vehicle.id = 42;
	vehicle.plate = "ABC-1234";
	vehicle.pos.latitude = 39.9042;
	vehicle.pos.longitude = 116.4074;
	vehicle.history[0].latitude = 39.9040;
	vehicle.history[0].longitude = 116.4070;
	vehicle.history[1].latitude = 39.9041;
	vehicle.history[1].longitude = 116.4072;
	vehicle.history[2].latitude = 39.9042;
	vehicle.history[2].longitude = 116.4074;
	ret = writeb(h, "vehicle1", &vehicle, sizeof(vehicle), &err);
	assert(ret);

	ret = writeq(h, "MotorStatusQueue", &motorStatus, sizeof(motorStatus), &err);
	assert(ret);

	MotorStatus motorStatus1;
	ret = readq(h, "MotorStatusQueue", &motorStatus1, sizeof(motorStatus1), &err);
	assert(ret);
	printf("motorStatus1.motor_name[0]=%s\n", motorStatus1.motor_name[0].c_str());

	printf("exit\n");

	return 0;
}