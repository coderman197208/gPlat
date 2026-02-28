#ifndef TYPE_HANDLE_H_
#define TYPE_HANDLE_H_

#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>

#include "../include/type_code.h"
#include "../include/struct_registry.h"

// --- 类型描述 ---

struct TypeInfo
{
	TypeCode code;
	int size;
	// 将 value buffer 的内容按该类型输出到 ostream
	void (*print)(std::ostream& os, const void* value);
};

// 辅助：生成 print 函数
template <typename T>
void PrintValue(std::ostream& os, const void* value)
{
	os << *reinterpret_cast<const T*>(value);
}

// Boolean 特殊处理
inline void PrintBool(std::ostream& os, const void* value)
{
	os << (*reinterpret_cast<const bool*>(value) ? "true" : "false");
}

// Char 特殊处理
inline void PrintChar(std::ostream& os, const void* value)
{
	os << *reinterpret_cast<const char*>(value);
}

// PodString 特殊处理（m_data 在 offset 0，对象地址即 C 字符串地址）
inline void PrintString(std::ostream& os, const void* value)
{
	os << '"' << reinterpret_cast<const char*>(value) << '"';
}

// --- 注册表 ---

// 名称 -> TypeInfo  (如 "Boolean" -> {Boolean, 1, PrintBool})
inline const std::unordered_map<std::string, TypeInfo>& GetTypeByName()
{
	static const std::unordered_map<std::string, TypeInfo> table = {
		{"Boolean", {Boolean, (int)sizeof(bool),     PrintBool}},
		{"Char",    {Char,    (int)sizeof(char),     PrintChar}},
		{"Int16",   {Int16,   (int)sizeof(int16_t),  PrintValue<int16_t>}},
		{"UInt16",  {UInt16,  (int)sizeof(uint16_t), PrintValue<uint16_t>}},
		{"Int32",   {Int32,   (int)sizeof(int32_t),  PrintValue<int32_t>}},
		{"UInt32",  {UInt32,  (int)sizeof(uint32_t), PrintValue<uint32_t>}},
		{"Int64",   {Int64,   (int)sizeof(int64_t),  PrintValue<int64_t>}},
		{"UInt64",  {UInt64,  (int)sizeof(uint64_t), PrintValue<uint64_t>}},
		{"Single",  {Single,  (int)sizeof(float),    PrintValue<float>}},
		{"Double",  {Double,  (int)sizeof(double),   PrintValue<double>}},
		{"String",  {String,  0,                      PrintString}},  // size=0: 实际大小由 FieldInfo.size 提供
		{"Struct",  {Struct,  0,                      nullptr}},     // 嵌套 struct，由 FieldInfo.struct_info 处理
	};
	return table;
}

// TypeCode -> TypeInfo  (用于 SelectItem 等需要根据 typecode 打印值的场景)
inline const std::unordered_map<int, TypeInfo>& GetTypeByCode()
{
	static const std::unordered_map<int, TypeInfo> table = {
		{Boolean, {Boolean, (int)sizeof(bool),     PrintBool}},
		{Char,    {Char,    (int)sizeof(char),     PrintChar}},
		{Int16,   {Int16,   (int)sizeof(int16_t),  PrintValue<int16_t>}},
		{UInt16,  {UInt16,  (int)sizeof(uint16_t), PrintValue<uint16_t>}},
		{Int32,   {Int32,   (int)sizeof(int32_t),  PrintValue<int32_t>}},
		{UInt32,  {UInt32,  (int)sizeof(uint32_t), PrintValue<uint32_t>}},
		{Int64,   {Int64,   (int)sizeof(int64_t),  PrintValue<int64_t>}},
		{UInt64,  {UInt64,  (int)sizeof(uint64_t), PrintValue<uint64_t>}},
		{Single,  {Single,  (int)sizeof(float),    PrintValue<float>}},
		{Double,  {Double,  (int)sizeof(double),   PrintValue<double>}},
		{String,  {String,  0,                      PrintString}},  // size=0: 实际大小由 FieldInfo.size 提供
		{Struct,  {Struct,  0,                      nullptr}},     // 嵌套 struct，由 FieldInfo.struct_info 处理
	};
	return table;
}

// S7 PLC 类型名 -> 我们的类型名
inline const std::unordered_map<std::string, std::string>& GetS7TypeMap()
{
	static const std::unordered_map<std::string, std::string> table = {
		{"BOOL",   "Boolean"},
		{"INT",    "Int16"},
		{"DINT",   "Int32"},
		{"WORD",   "UInt16"},
		{"DWORD",  "UInt32"},
		{"REAL",   "Single"},
		{"STRING", "Char"},
	};
	return table;
}

// --- 便捷查找函数 ---

// 通过类型名查找，找不到返回 nullptr
inline const TypeInfo* FindTypeByName(const std::string& name)
{
	auto& table = GetTypeByName();
	auto it = table.find(name);
	return (it != table.end()) ? &it->second : nullptr;
}

// 通过 TypeCode 查找，找不到返回 nullptr
inline const TypeInfo* FindTypeByCode(int code)
{
	auto& table = GetTypeByCode();
	auto it = table.find(code);
	return (it != table.end()) ? &it->second : nullptr;
}

// 通过 S7 类型名查找对应的我们的类型名，找不到返回空字符串
inline std::string MapS7Type(const std::string& s7type)
{
	auto& table = GetS7TypeMap();
	auto it = table.find(s7type);
	return (it != table.end()) ? it->second : "";
}

#endif // TYPE_HANDLE_H_
