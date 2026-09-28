#ifndef RECORD_INPUT_H_
#define RECORD_INPUT_H_

#include <string>
#include <vector>

#include "../include/struct_reflect.h"

// assignments 每项形如 "<path>=<value>"，path: field | field[i] | field.member | field[i].member
// 非 struct 数组可不带下标整体赋值 "field=v1,v2,..."；未赋值的字段为零
bool BuildRecord(const StructInfo& info, const std::vector<std::string>& assignments,
	std::vector<char>& record, std::string& error);

#endif // RECORD_INPUT_H_
