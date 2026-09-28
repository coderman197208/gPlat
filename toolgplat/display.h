#ifndef DISPLAY_H_
#define DISPLAY_H_

#include <string>

#include "../include/higplat.h"
#include "../include/struct_reflect.h"

void PrintTag(int conn, const std::string& tagName);
void PrintBoardInfo(int conn);
void PrintTypes();

// type 为 nullptr 表示记录类型未在本地注册
void PrintQueueInfo(const std::string& queueName, const QUEUE_HEAD& head, const std::string& typeName, const StructInfo* type);
void PrintQueueRecord(const RECORD_HEAD& recordHead, const StructInfo* type, const char* data, int size);

#endif // DISPLAY_H_
