#include "creation.h"

#include <fstream>
#include <iostream>

#include "../include/higplat.h"
#include "text_util.h"
#include "type_handle.h"

namespace
{

constexpr int kMaxDataSize = 16000;          // 单个 tag 或队列记录的最大字节数
constexpr int kDefaultS7StringLength = 254;  // 与 s7ioserver 的 STRING 默认长度一致

struct ElementType
{
	std::string name;
	int32_t code;     // 内置 TypeCode 或 kStructTypeCode
	int size;
};

bool ResolveStructType(const std::string& name, ElementType& type)
{
	const StructInfo* si = FindStructByName(name);
	if (!si)
		return false;
	type = {name, kStructTypeCode, si->total_size};
	return true;
}

// typeSpec: 内置类型名 | String | 已注册 struct 名 | <类型名>$<单条大小>（本地未注册的自定义类型）
bool ResolveTagType(const std::string& typeSpec, ElementType& type)
{
	size_t dollar = typeSpec.find('$');
	if (dollar != std::string::npos)
	{
		type = {typeSpec.substr(0, dollar), kStructTypeCode, 0};
		return !type.name.empty() && ParseInt(typeSpec.substr(dollar + 1), type.size) && type.size > 0;
	}

	// String 类型的 tag 按 Char 数组存储
	const std::string name = (ToLower(typeSpec) == "string") ? "Char" : typeSpec;
	const TypeInfo* ti = FindTypeByName(name);
	if (ti && ti->size > 0)
	{
		type = {name, ti->code, ti->size};
		return true;
	}
	return ResolveStructType(name, type);
}

bool CreateTag(int conn, const std::string& tagName, const std::string& typeSpec, int arraySize)
{
	ElementType type;
	if (!ResolveTagType(typeSpec, type))
	{
		std::cout << "Unknown type '" << typeSpec << "'." << std::endl;
		return false;
	}

	const bool isArray = arraySize > 1;
	const long long tagSize = (long long)type.size * (isArray ? arraySize : 1);
	if (tagSize > kMaxDataSize)
	{
		std::cout << "Tag '" << tagName << "' size " << tagSize << " exceeds limit " << kMaxDataSize << "." << std::endl;
		return false;
	}

	std::vector<char> descriptor = BuildTypeDescriptor(type.code, isArray ? arraySize : 0, type.name);
	unsigned int err = 0;
	if (!createtag(conn, tagName.c_str(), (int)tagSize, descriptor.data(), (int)descriptor.size(), &err))
	{
		std::cout << "Create tag '" << tagName << "' failed, error code " << err << "." << std::endl;
		return false;
	}

	std::cout << "Tag '" << tagName << "' created: " << type.name;
	if (isArray)
		std::cout << "[" << arraySize << "]";
	std::cout << ", " << tagSize << " bytes." << std::endl;
	return true;
}

void PrintBatchSummary(const std::string& path, int succeeded, int failed)
{
	std::cout << path << ": 成功 " << succeeded << " 个, 失败 " << failed << " 个" << std::endl;
}

bool IsPlcConnectionKey(const std::string& key)
{
	return key == "ip" || key == "rack" || key == "slot" || key == "poll_interval";
}

} // namespace

bool CreateTagFromWords(int conn, const std::vector<std::string>& words)
{
	if (words.size() != 3 && words.size() != 4)
	{
		std::cout << "Usage: create <tagName> <typeName> [arraySize]" << std::endl;
		return false;
	}

	int arraySize = 0;
	if (words.size() == 4 && (!ParseInt(words[3], arraySize) || arraySize < 0))
	{
		std::cout << "Array size must be a non-negative integer: " << words[3] << std::endl;
		return false;
	}
	return CreateTag(conn, words[1], words[2], arraySize);
}

bool CreateQueue(int conn, const std::string& queueName, const std::string& typeName, int recordCount, bool shiftMode)
{
	ElementType type;
	if (!ResolveStructType(typeName, type))
	{
		if (FindTypeByName(typeName))
			std::cout << "不支持创建简单和字符串类型的队列，请定义队列的数据结构！" << std::endl;
		else
			std::cout << "Unknown type '" << typeName << "'." << std::endl;
		return false;
	}

	if (type.size > kMaxDataSize)
	{
		std::cout << "Record size " << type.size << " exceeds limit " << kMaxDataSize << "." << std::endl;
		return false;
	}

	// 队列记录不支持数组类型
	std::vector<char> descriptor = BuildTypeDescriptor(type.code, 0, type.name);
	unsigned int err = 0;
	if (!createqueue(conn, queueName.c_str(), type.size, recordCount, shiftMode ? SHIFT_MODE : NORMAL_MODE,
		descriptor.data(), (int)descriptor.size(), &err))
	{
		if (err == ERROR_ALREADY_LOAD)
			std::cout << "'" << queueName << "' already exists." << std::endl;
		else if (err == ERROR_INVALID_PARAMETER)
			std::cout << "Invalid queue name '" << queueName << "'." << std::endl;
		else
			std::cout << "Create queue '" << queueName << "' failed, error code " << err << "." << std::endl;
		return false;
	}

	std::cout << "Queue '" << queueName << "' created: " << type.name << ", " << type.size << " bytes x "
		<< recordCount << (shiftMode ? ", shift mode." : ".") << std::endl;
	return true;
}

void CreateTagsFromScriptFile(int conn, const std::string& path)
{
	std::ifstream file(path);
	if (!file)
	{
		std::cout << "无法打开文件: " << path << std::endl;
		return;
	}

	int succeeded = 0;
	int failed = 0;
	std::string line;
	while (std::getline(file, line))
	{
		line = StripComment(line);
		if (line.empty() || line[0] == ';')
			continue;

		std::vector<std::string> words = SplitWords(line);
		if (words[0] != "create")
			continue;

		if (CreateTagFromWords(conn, words))
			++succeeded;
		else
			++failed;
	}
	PrintBatchSummary(path, succeeded, failed);
}

void CreateTagsFromConfigFile(int conn, const std::string& path)
{
	std::ifstream file(path);
	if (!file)
	{
		std::cout << "无法打开文件: " << path << std::endl;
		return;
	}

	int succeeded = 0;
	int failed = 0;
	bool inPlcSection = false;
	std::string line;
	while (std::getline(file, line))
	{
		line = StripComment(line);
		if (line.empty())
			continue;

		if (line[0] == '[')
		{
			size_t closeBracket = line.find(']');
			if (closeBracket != std::string::npos)
				inPlcSection = Trim(line.substr(1, closeBracket - 1)) != "general";
			continue;
		}
		if (!inPlcSection)
			continue;

		size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		const std::string tagName = Trim(line.substr(0, eq));
		const std::string value = Trim(line.substr(eq + 1));
		if (tagName.empty() || value.empty() || IsPlcConnectionKey(tagName))
			continue;

		// value: 区域, DB号, 偏移, 数据类型 [, 最大长度]
		std::vector<std::string> fields = SplitAndTrim(value, ',');
		if (fields.size() < 4)
			continue;

		const std::string s7type = ToUpper(fields[3]);
		const std::string typeName = MapS7Type(s7type);
		if (typeName.empty())
		{
			std::cout << "未知数据类型 '" << s7type << "' (tag: " << tagName << ")，跳过" << std::endl;
			continue;
		}

		int arraySize = 0;
		if (s7type == "STRING")
		{
			arraySize = kDefaultS7StringLength;
			if (fields.size() >= 5 && !fields[4].empty() && !ParseInt(fields[4], arraySize))
			{
				std::cout << "STRING类型 '" << tagName << "' 最大长度无效: " << fields[4] << "，跳过" << std::endl;
				continue;
			}
		}

		if (CreateTag(conn, tagName, typeName, arraySize))
			++succeeded;
		else
			++failed;
	}
	PrintBatchSummary(path, succeeded, failed);
}
