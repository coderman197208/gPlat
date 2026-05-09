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

struct SensorData {
	int32_t       temperature;
	int32_t       humidity;
	double        pressure;
	bool          alarm;
	PodString<20> location;
};

REGISTER_STRUCT(SensorData,
	FIELD_DESC(Int32,   SensorData, temperature),
	FIELD_DESC(Int32,   SensorData, humidity),
	FIELD_DESC(Double,  SensorData, pressure),
	FIELD_DESC(Boolean, SensorData, alarm),
	FIELD_DESC_STRING(SensorData, location)
)

struct MotorStatus {
	float         speed[3];
	float         current;
	int32_t       error_code;
	uint32_t      run_count;
	PodString<16> motor_name[3];
};

REGISTER_STRUCT(MotorStatus,
	FIELD_DESC_ARRAY(Single,  MotorStatus, speed, 3),
	FIELD_DESC(Single,  MotorStatus, current),
	FIELD_DESC(Int32,   MotorStatus, error_code),
	FIELD_DESC(UInt32,  MotorStatus, run_count),
	FIELD_DESC_STRING_ARRAY(MotorStatus, motor_name, 3)
)

// 嵌套 struct 示例（内层：仅含基本类型，供外层引用）

struct GPSPosition {
	double latitude;
	double longitude;
};

REGISTER_STRUCT(GPSPosition,
	FIELD_DESC(Double, GPSPosition, latitude),
	FIELD_DESC(Double, GPSPosition, longitude)
)

struct Vehicle {
	int32_t       id;
	GPSPosition   pos;
	GPSPosition   history[3];
	PodString<16> plate;
};

REGISTER_STRUCT(Vehicle,
	FIELD_DESC(Int32, Vehicle, id),
	FIELD_DESC_STRUCT(Vehicle, pos, GPSPosition),
	FIELD_DESC_STRUCT_ARRAY(Vehicle, history, GPSPosition, 3),
	FIELD_DESC_STRING(Vehicle, plate)
)

//下面是GT4_APP项目的用户自定义struct定义

//设定生产计划命令
struct SetFeedNumCmd
{
	int feed_num; // 投料支数
};

REGISTER_STRUCT(SetFeedNumCmd,
	FIELD_DESC(Int32, SetFeedNumCmd, feed_num)
)

//移动管子命令
struct MoveTubeCmd
{
	PodString<20> from; // 来源工位
	PodString<20> to;   // 目标工位
};

REGISTER_STRUCT(MoveTubeCmd,
	FIELD_DESC_STRING(MoveTubeCmd, from),
	FIELD_DESC_STRING(MoveTubeCmd, to)
)

struct ModifyTubeCmd {
	int seq_no; // 序列号，确保管子的唯一性和顺序
	PodString<20> position_name; // 工位名称
	PodString<20> order_no; // 合同号
	PodString<20> item_no; // 项目号
	PodString<20> roll_no; // 轧批号
	PodString<20> melt_no; // 炉号
	PodString<20> lot_no; // 试批号
	int tube_no; // 管号
	int flow_no; // 流水号
	double length; // 长度(米)
	double weight; // 重量(KG)
	bool length_ok; // 长度合格
	bool weight_ok; // 重量合格
	PodString<20> lotno_coupling; // 接箍批号
	PodString<20> meltno_coupling; // 接箍炉号
};

REGISTER_STRUCT(ModifyTubeCmd,
	FIELD_DESC(Int32, ModifyTubeCmd, seq_no),
	FIELD_DESC_STRING(ModifyTubeCmd, position_name),
	FIELD_DESC_STRING(ModifyTubeCmd, order_no),
	FIELD_DESC_STRING(ModifyTubeCmd, item_no),
	FIELD_DESC_STRING(ModifyTubeCmd, roll_no),
	FIELD_DESC_STRING(ModifyTubeCmd, melt_no),
	FIELD_DESC_STRING(ModifyTubeCmd, lot_no),
	FIELD_DESC(Int32, ModifyTubeCmd, tube_no),
	FIELD_DESC(Int32, ModifyTubeCmd, flow_no),
	FIELD_DESC(Double, ModifyTubeCmd, length),
	FIELD_DESC(Double, ModifyTubeCmd, weight),
	FIELD_DESC(Boolean, ModifyTubeCmd, length_ok),
	FIELD_DESC(Boolean, ModifyTubeCmd, weight_ok),
	FIELD_DESC_STRING(ModifyTubeCmd, lotno_coupling),
	FIELD_DESC_STRING(ModifyTubeCmd, meltno_coupling)
)

struct DeleteTubeCmd {
	int seq_no; // 序列号，确保管子的唯一性和顺序
	PodString<20> position_name; // 工位名称
};

REGISTER_STRUCT(DeleteTubeCmd,
	FIELD_DESC(Int32, DeleteTubeCmd, seq_no),
	FIELD_DESC_STRING(DeleteTubeCmd, position_name)
)


struct SetCurrentContractCmd {
  PodString<20> order_no; // 合同号
  PodString<20> item_no; // 项目号
};

REGISTER_STRUCT(SetCurrentContractCmd,
	FIELD_DESC_STRING(SetCurrentContractCmd, order_no),
	FIELD_DESC_STRING(SetCurrentContractCmd, item_no)
)

struct StartSprayEvent
{
    PodString<20> order_no; // 合同号
    PodString<20> item_no; // 项目号
    int tube_no; // 管号
    int flow_no; // 流水号
    PodString<20> melt_no; // 炉号
    PodString<20> lot_no; // 试批号
    double length; // 长度(米)
    double weight; // 重量(KG)
    double theory_weight; // 理论重量
    bool length_ok; // 长度合格
    bool weight_ok; // 重量合格
};

REGISTER_STRUCT(StartSprayEvent,
	FIELD_DESC_STRING(StartSprayEvent, order_no),
	FIELD_DESC_STRING(StartSprayEvent, item_no),
	FIELD_DESC(Int32, StartSprayEvent, tube_no),
	FIELD_DESC(Int32, StartSprayEvent, flow_no),
	FIELD_DESC_STRING(StartSprayEvent, melt_no),
	FIELD_DESC_STRING(StartSprayEvent, lot_no),
	FIELD_DESC(Double, StartSprayEvent, length),
	FIELD_DESC(Double, StartSprayEvent, weight),
	FIELD_DESC(Double, StartSprayEvent, theory_weight),
	FIELD_DESC(Boolean, StartSprayEvent, length_ok),
	FIELD_DESC(Boolean, StartSprayEvent, weight_ok)
)



#endif // USER_TYPES_H_
