#ifndef TYPE_HANDLE_H_
#define TYPE_HANDLE_H_

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "../include/type_code.h"
#include "../include/struct_registry.h"

// --- 内置类型 ---

struct TypeInfo
{
	const char* name;
	TypeCode code;
	int size;                                              // 0: 大小由 FieldInfo.size 决定
	void (*print)(std::ostream& os, const void* value);    // nullptr: 不能直接打印
};

template <typename T>
void PrintValue(std::ostream& os, const void* value)
{
	os << *reinterpret_cast<const T*>(value);
}

inline void PrintBool(std::ostream& os, const void* value)
{
	os << (*reinterpret_cast<const bool*>(value) ? "true" : "false");
}

// PodString 的 m_data 位于 offset 0，对象地址即 C 字符串地址
inline void PrintPodString(std::ostream& os, const void* value)
{
	os << '"' << reinterpret_cast<const char*>(value) << '"';
}

inline const std::vector<TypeInfo>& GetBuiltinTypes()
{
	static const std::vector<TypeInfo> types = {
		{"Boolean", Boolean, (int)sizeof(bool),     PrintBool},
		{"Char",    Char,    (int)sizeof(char),     PrintValue<char>},
		{"Int16",   Int16,   (int)sizeof(int16_t),  PrintValue<int16_t>},
		{"UInt16",  UInt16,  (int)sizeof(uint16_t), PrintValue<uint16_t>},
		{"Int32",   Int32,   (int)sizeof(int32_t),  PrintValue<int32_t>},
		{"UInt32",  UInt32,  (int)sizeof(uint32_t), PrintValue<uint32_t>},
		{"Int64",   Int64,   (int)sizeof(int64_t),  PrintValue<int64_t>},
		{"UInt64",  UInt64,  (int)sizeof(uint64_t), PrintValue<uint64_t>},
		{"Single",  Single,  (int)sizeof(float),    PrintValue<float>},
		{"Double",  Double,  (int)sizeof(double),   PrintValue<double>},
		{"String",  String,  0,                     PrintPodString},
		{"Struct",  Struct,  0,                     nullptr},
	};
	return types;
}

inline const TypeInfo* FindTypeByName(const std::string& name)
{
	const auto& types = GetBuiltinTypes();
	auto it = std::find_if(types.begin(), types.end(),
		[&](const TypeInfo& t) { return name == t.name; });
	return (it != types.end()) ? &*it : nullptr;
}

inline const TypeInfo* FindTypeByCode(int code)
{
	const auto& types = GetBuiltinTypes();
	auto it = std::find_if(types.begin(), types.end(),
		[&](const TypeInfo& t) { return code == t.code; });
	return (it != types.end()) ? &*it : nullptr;
}

// --- 类型描述符 ---
// createtag / createqueue / readtype 使用的格式:
//   [int32 typecode][int32 arraysize][类名 '\0']，类名仅在 typecode == kStructTypeCode 时存在

constexpr int32_t kStructTypeCode = -1;

struct TypeDescriptorHeader
{
	int32_t typecode;
	int32_t arraysize;    // 0: 非数组
};

inline std::vector<char> BuildTypeDescriptor(int32_t typecode, int32_t arraysize, const std::string& className)
{
	const TypeDescriptorHeader header{typecode, arraysize};
	const char* raw = reinterpret_cast<const char*>(&header);
	std::vector<char> descriptor(raw, raw + sizeof(header));
	if (typecode == kStructTypeCode)
		descriptor.insert(descriptor.end(), className.c_str(), className.c_str() + className.size() + 1);
	return descriptor;
}

constexpr int kTypeDescriptorBufferSize = 2048;

// className 仅在 typecode == kStructTypeCode 时被赋值
inline bool ParseTypeDescriptor(const char* descriptor, int size, TypeDescriptorHeader& header, std::string& className)
{
	if (size < (int)sizeof(TypeDescriptorHeader))
		return false;
	memcpy(&header, descriptor, sizeof(header));
	if (header.typecode == kStructTypeCode)
	{
		const char* name = descriptor + sizeof(header);
		className.assign(name, strnlen(name, size - sizeof(header)));
	}
	return true;
}

// --- S7 PLC 类型 ---

// S7 类型名（大写）-> 内置类型名，找不到返回空字符串
inline std::string MapS7Type(const std::string& s7type)
{
	static const std::vector<std::pair<std::string, std::string>> table = {
		{"BOOL",   "Boolean"},
		{"INT",    "Int16"},
		{"DINT",   "Int32"},
		{"WORD",   "UInt16"},
		{"DWORD",  "UInt32"},
		{"REAL",   "Single"},
		{"STRING", "Char"},
	};
	for (const auto& [s7, builtin] : table)
		if (s7 == s7type)
			return builtin;
	return "";
}

#endif // TYPE_HANDLE_H_
