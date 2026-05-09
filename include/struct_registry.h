#ifndef STRUCT_REGISTRY_H_
#define STRUCT_REGISTRY_H_

#include "struct_reflect.h"
#include "user_types.h"
#include <unordered_map>
#include <string>

// 注册宏：将 GetStructInfo_XXX() 加入全局表
#define REG(NAME) { #NAME, &GetStructInfo_##NAME() }

inline const std::unordered_map<std::string, const StructInfo*>& GetStructRegistry()
{
	static const std::unordered_map<std::string, const StructInfo*> table = {
		REG(SensorData),
		REG(MotorStatus),
		REG(GPSPosition),
		REG(Vehicle),
		REG(SetFeedNumCmd),
		REG(MoveTubeCmd),
		REG(ModifyTubeCmd),
		REG(DeleteTubeCmd),
		REG(SetCurrentContractCmd),
		REG(StartSprayEvent),
		// 新增类型在此添加一行
	};
	return table;
}

#undef REG

// 便捷查找函数
inline const StructInfo* FindStructByName(const std::string& name)
{

	auto& table = GetStructRegistry();
	auto it = table.find(name);
	return (it != table.end()) ? it->second : nullptr;
}

#endif // STRUCT_REGISTRY_H_
