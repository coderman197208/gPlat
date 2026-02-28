#ifndef STRUCT_REFLECT_H_
#define STRUCT_REFLECT_H_

#include <cstddef>      // offsetof
#include <cstdint>
#include <type_traits>  // is_trivially_copyable_v
#include "type_code.h"

// TypeCode -> C++ 原生类型映射
#define CTYPE_Boolean   bool
#define CTYPE_Char      char
#define CTYPE_Int16     int16_t
#define CTYPE_UInt16    uint16_t
#define CTYPE_Int32     int32_t
#define CTYPE_UInt32    uint32_t
#define CTYPE_Int64     int64_t
#define CTYPE_UInt64    uint64_t
#define CTYPE_Single    float
#define CTYPE_Double    double

struct StructInfo;  // 前向声明，供 FieldInfo 使用

// 单个字段的描述信息
struct FieldInfo
{
	const char*       name;          // 字段名（如 "temperature"）
	TypeCode          type;          // 类型码（复用 TypeCode 枚举）
	int               size;          // 字段总大小（字节）
	int               offset;        // 相对 struct 起始的偏移量
	int               element_count; // 元素个数（1=标量，>1=数组）
	const StructInfo* struct_info;   // type==Struct 时指向内层描述，否则 nullptr
};

// 一个 struct 的完整描述
struct StructInfo
{
	const char*       name;         // struct 名（如 "SensorData"）
	int               total_size;   // sizeof(struct)
	int               field_count;  // 字段数量
	const FieldInfo*  fields;       // 字段数组指针
};

// 字段描述宏（方案 B：两阶段手写）
#define FIELD_DESC(TYPE, STRUCT, NAME) \
	{ #NAME, TYPE, (int)sizeof(CTYPE_##TYPE), (int)offsetof(STRUCT, NAME), 1, nullptr }

// 数组字段描述宏
#define FIELD_DESC_ARRAY(TYPE, STRUCT, NAME, COUNT) \
	{ #NAME, TYPE, (int)sizeof(CTYPE_##TYPE) * COUNT, (int)offsetof(STRUCT, NAME), COUNT, nullptr }

// PodString 字段描述宏
// PodString<N> 的容量各异，无法用 CTYPE_String 统一映射，
// 改用 sizeof(STRUCT::NAME) 自动获取实际大小，TypeCode 统一为 String
#define FIELD_DESC_STRING(STRUCT, NAME) \
	{ #NAME, String, (int)sizeof(decltype(STRUCT::NAME)), (int)offsetof(STRUCT, NAME), 1, nullptr }

// PodString 数组字段描述宏
#define FIELD_DESC_STRING_ARRAY(STRUCT, NAME, COUNT) \
	{ #NAME, String, (int)sizeof(decltype(STRUCT::NAME)), (int)offsetof(STRUCT, NAME), COUNT, nullptr }

// 嵌套 struct 字段描述宏（限一层嵌套）
#define FIELD_DESC_STRUCT(STRUCT, NAME, NESTED) \
	{ #NAME, Struct, (int)sizeof(NESTED), (int)offsetof(STRUCT, NAME), 1, \
	  &GetStructInfo_##NESTED() }

// 嵌套 struct 数组字段描述宏
#define FIELD_DESC_STRUCT_ARRAY(STRUCT, NAME, NESTED, COUNT) \
	{ #NAME, Struct, (int)sizeof(NESTED) * COUNT, (int)offsetof(STRUCT, NAME), COUNT, \
	  &GetStructInfo_##NESTED() }

// 注册宏：生成 GetStructInfo_XXX() 函数
#define REGISTER_STRUCT(NAME, ...) \
	static_assert(std::is_trivially_copyable_v<NAME>, \
		#NAME " must be trivially copyable for Board storage"); \
	inline const StructInfo& GetStructInfo_##NAME() { \
		static const FieldInfo fields[] = { __VA_ARGS__ }; \
		static const StructInfo info = { \
			#NAME, (int)sizeof(NAME), \
			(int)(sizeof(fields)/sizeof(fields[0])), fields \
		}; \
		return info; \
	}

#endif // STRUCT_REFLECT_H_
