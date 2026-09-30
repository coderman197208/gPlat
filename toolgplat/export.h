#ifndef EXPORT_H_
#define EXPORT_H_

#include <ostream>
#include <string>
#include <vector>

struct CreateScript
{
	std::vector<std::string> queueLines;   // 按队列名排序
	std::vector<std::string> tagLines;     // 按 tag 名排序
};

// 由服务端现有的队列和 tag 元数据还原 create 命令；queueNames 需已排序
bool CollectCreateScript(int conn, const std::vector<std::string>& queueNames, CreateScript& script);

// source: 服务端地址，写入注释头
void WriteCreateScript(std::ostream& os, const std::string& source, const CreateScript& script);

#endif // EXPORT_H_
