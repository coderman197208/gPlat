#ifndef USER_TYPES_H_
#define USER_TYPES_H_

#include "struct_reflect.h"
#include "podstring.h"

// ============================================================
// 用户自定义 struct 定义
// 使用方法：
//   1. 用 #pragma pack(push, 8) 定义 struct
//   2. 用 REGISTER_STRUCT 注册元数据
//   3. 在 struct_registry.h 的 GetStructRegistry() 中添加 REG(XXX)
// ============================================================

#pragma pack(push, 8)

struct SensorData {
	int32_t       temperature;
	int32_t       humidity;
	double        pressure;
	bool          alarm;
	PodString<20> location;
};

#pragma pack(pop)

REGISTER_STRUCT(SensorData,
	FIELD_DESC(Int32,   SensorData, temperature),
	FIELD_DESC(Int32,   SensorData, humidity),
	FIELD_DESC(Double,  SensorData, pressure),
	FIELD_DESC(Boolean, SensorData, alarm),
	FIELD_DESC_STRING(SensorData, location)
)

#pragma pack(push, 8)

struct MotorStatus {
	float         speed[3];
	float         current;
	int32_t       error_code;
	uint32_t      run_count;
	PodString<16> motor_name[3];
};

#pragma pack(pop)

REGISTER_STRUCT(MotorStatus,
	FIELD_DESC_ARRAY(Single,  MotorStatus, speed, 3),
	FIELD_DESC(Single,  MotorStatus, current),
	FIELD_DESC(Int32,   MotorStatus, error_code),
	FIELD_DESC(UInt32,  MotorStatus, run_count),
	FIELD_DESC_STRING_ARRAY(MotorStatus, motor_name, 3)
)

#endif // USER_TYPES_H_
