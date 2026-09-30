#include "export.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <iostream>
#include <utility>

#include "../include/higplat.h"
#include "type_handle.h"

namespace
{

constexpr int kTagPageSize = 16384;   // 与服务端 MAXMSGLEN 一致

// create 命令中的 "<typeName> [arraySize]"
struct TypeSpec
{
	std::string name;
	int arraySize = 0;
	bool isStruct = false;

	std::string Text() const { return arraySize > 0 ? name + " " + std::to_string(arraySize) : name; }
};

// dataSize: tag 大小或队列单条记录大小
bool RestoreTypeSpec(const char* descriptor, int descriptorSize, int dataSize, TypeSpec& spec)
{
	TypeDescriptorHeader header;
	std::string className;
	if (dataSize <= 0 || !ParseTypeDescriptor(descriptor, descriptorSize, header, className) || header.arraysize < 0)
		return false;

	// 与 CreateTag 一致：arraySize <= 1 按非数组创建
	spec.arraySize = (header.arraysize > 1) ? header.arraysize : 0;
	const int count = (spec.arraySize > 0) ? spec.arraySize : 1;
	if (header.typecode == kStructTypeCode)
	{
		if (className.empty() || dataSize % count != 0)
			return false;
		const int elementSize = dataSize / count;
		const StructInfo* info = FindStructByName(className);
		spec.name = (info && info->total_size == elementSize) ? className : className + "$" + std::to_string(elementSize);
		spec.isStruct = true;
		return true;
	}

	const TypeInfo* type = FindTypeByCode(header.typecode);
	if (!type || type->size == 0 || type->size * count != dataSize)
		return false;
	spec.name = (type->code == Char && spec.arraySize > 0) ? "string" : type->name;
	return true;
}

// SplitWords 支持双引号，含空白的名字需加引号才能被重新创建
std::string QuoteName(const std::string& name)
{
	const bool hasSpace = std::any_of(name.begin(), name.end(), [](unsigned char c) { return std::isspace(c); });
	return hasSpace ? "\"" + name + "\"" : name;
}

const char* MissingTypeReason(int typeSize)
{
	return typeSize > 0 ? "unsupported type" : "no type info";
}

std::string QueueLine(int conn, const std::string& name)
{
	QUEUE_HEAD head{};
	unsigned int err = 0;
	if (!readhead(conn, name.c_str(), &head, &err))
		return "# queue " + name + ": read head failed, error code " + std::to_string(err);

	char descriptor[kTypeDescriptorBufferSize];
	int size = 0;
	TypeSpec spec;
	const bool restored = readtype(conn, name.c_str(), "", descriptor, sizeof(descriptor), &size, &err)
		&& RestoreTypeSpec(descriptor, std::min(size, (int)sizeof(descriptor)), head.size, spec)
		&& spec.isStruct && spec.arraySize == 0;
	if (!restored)
		return "# queue " + name + ": " + MissingTypeReason(size) + ", " + std::to_string(head.size) + " bytes x "
			+ std::to_string(head.num) + (head.operateMode == SHIFT_MODE ? ", shift mode" : "");

	return "create queue " + QuoteName(name) + " " + spec.Text() + " " + std::to_string(head.num)
		+ (head.operateMode == SHIFT_MODE ? " shift" : "");
}

std::string TagLine(const std::string& name, const TAG_META& meta, const char* descriptor)
{
	TypeSpec spec;
	if (meta.typesize > 0 && RestoreTypeSpec(descriptor, meta.typesize, meta.itemsize, spec))
		return "create " + QuoteName(name) + " " + spec.Text();
	return "# tag " + name + ": " + MissingTypeReason(meta.typesize) + ", " + std::to_string(meta.itemsize) + " bytes";
}

bool CollectTagLines(int conn, std::vector<std::string>& lines)
{
	std::vector<std::pair<std::string, std::string>> tags;   // 名字, 命令
	std::vector<char> page(kTagPageSize);
	for (int start = 0; start >= 0;)
	{
		int bytes = 0;
		int count = 0;
		int next = -1;
		unsigned int err = 0;
		if (!listtags(conn, start, page.data(), (int)page.size(), &bytes, &count, &next, &err))
		{
			std::cout << "List tags failed, error code " << err << "." << std::endl;
			return false;
		}

		const char* p = page.data();
		const char* end = p + bytes;
		for (int i = 0; i < count && end - p > (long)sizeof(TAG_META); i++)
		{
			TAG_META meta;
			memcpy(&meta, p, sizeof(meta));
			p += sizeof(meta);
			const std::string name(p, strnlen(p, end - p));
			p += name.size() + 1;
			if (meta.typesize < 0 || meta.typesize > end - p)
				break;
			tags.emplace_back(name, TagLine(name, meta, p));
			p += meta.typesize;
		}
		start = next;
	}

	std::sort(tags.begin(), tags.end());
	for (auto& tag : tags)
		lines.push_back(std::move(tag.second));
	return true;
}

} // namespace

bool CollectCreateScript(int conn, const std::vector<std::string>& queueNames, CreateScript& script)
{
	for (const std::string& name : queueNames)
		script.queueLines.push_back(QueueLine(conn, name));
	return CollectTagLines(conn, script.tagLines);
}

void WriteCreateScript(std::ostream& os, const std::string& source, const CreateScript& script)
{
	const std::time_t now = std::time(nullptr);
	std::tm local{};
	localtime_r(&now, &local);
	char timeText[32];
	std::strftime(timeText, sizeof(timeText), "%Y-%m-%d %H:%M:%S", &local);

	os << "# gPlat create script, exported from " << source << " at " << timeText << std::endl;
	os << "# " << script.queueLines.size() << " queue(s), " << script.tagLines.size() << " tag(s)" << std::endl;
	for (const auto* lines : {&script.queueLines, &script.tagLines})
	{
		if (lines->empty())
			continue;
		os << std::endl;
		for (const std::string& line : *lines)
			os << line << std::endl;
	}
}
