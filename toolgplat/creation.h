#ifndef CREATION_H_
#define CREATION_H_

#include <string>
#include <vector>

// words: create <tagName> <typeName> [arraySize]
bool CreateTagFromWords(int conn, const std::vector<std::string>& words);

// 队列元素只能是 struct 类型（已注册或 <类型名>$<单条大小>）
bool CreateQueue(int conn, const std::string& queueName, const std::string& typeName, int recordCount, bool shiftMode);

// words: create queue <queueName> <typeName> <recordCount> [shift]
bool CreateQueueFromWords(int conn, const std::vector<std::string>& words);

// 每行一条 create 命令（tag 或 queue），'#' 或 ';' 开头为注释
void CreateTagsFromScriptFile(int conn, const std::string& path);

// s7ioserver 的 INI 配置，为每个 PLC section 中的 tag 映射创建对应 tag
void CreateTagsFromConfigFile(int conn, const std::string& path);

#endif // CREATION_H_
