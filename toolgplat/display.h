#ifndef DISPLAY_H_
#define DISPLAY_H_

#include <string>

#include "../include/higplat.h"
#include "../include/struct_reflect.h"
#include "type_handle.h"

// 失败时输出错误信息
bool ReadTagType(int conn, const std::string& tagName, TypeDescriptorHeader& header, std::string& className);
void PrintTag(int conn, const std::string& tagName);
// data 为 tag 的完整内容；字符串 tag 为以 '\0' 结尾或长度为 size 的字符串
void PrintTagData(const TypeDescriptorHeader& header, const std::string& className, const char* data, int size);
void PrintBoardInfo(int conn);
void PrintTypes();

// type 为 nullptr 表示记录类型未在本地注册
void PrintQueueInfo(const std::string& queueName, const QUEUE_HEAD& head, const std::string& typeName, const StructInfo* type);
void PrintQueueRecord(const RECORD_HEAD& recordHead, const StructInfo* type, const char* data, int size);

#endif // DISPLAY_H_
