#ifndef CREATION_H_
#define CREATION_H_

#include <string>
#include <vector>

// words: create <tagName> <typeName> [arraySize]
bool CreateTagFromWords(int conn, const std::vector<std::string>& words);

// 队列元素只能是已注册的 struct 类型
bool CreateQueue(int conn, const std::string& queueName, const std::string& typeName, int recordCount, bool shiftMode);

// 每行一条 "create <tagName> <typeName> [arraySize]"，'#' 或 ';' 开头为注释
void CreateTagsFromScriptFile(int conn, const std::string& path);

// s7ioserver 的 INI 配置，为每个 PLC section 中的 tag 映射创建对应 tag
void CreateTagsFromConfigFile(int conn, const std::string& path);

#endif // CREATION_H_
