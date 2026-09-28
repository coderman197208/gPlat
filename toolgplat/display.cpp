#include "display.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <vector>

#include "../include/higplat.h"
#include "type_handle.h"

namespace
{

constexpr int kMaxHexDumpBytes = 64;
constexpr int kTypeDescriptorBufferSize = 2048;

void PrintHex(std::ostream& os, const char* data, int size)
{
	const auto* bytes = reinterpret_cast<const unsigned char*>(data);
	const char oldFill = os.fill('0');
	for (int i = 0; i < size && i < kMaxHexDumpBytes; i++)
		os << std::hex << std::setw(2) << (int)bytes[i] << ' ';
	os << std::dec;
	os.fill(oldFill);
}

// count > 1 时输出为 [a, b, c]
void PrintElements(std::ostream& os, const TypeInfo& type, const char* data, int elementSize, int count)
{
	if (count == 1)
	{
		type.print(os, data);
		return;
	}
	os << "[";
	for (int i = 0; i < count; i++)
	{
		if (i > 0)
			os << ", ";
		type.print(os, data + i * elementSize);
	}
	os << "]";
}

void PrintFieldValue(std::ostream& os, const FieldInfo& field, const char* data)
{
	const TypeInfo* type = FindTypeByCode(field.type);
	if (type && type->print)
		PrintElements(os, *type, data, field.size / field.element_count, field.element_count);
	else
		PrintHex(os, data, field.size);
}

void PrintFields(const StructInfo& info, const char* base, int indent)
{
	const std::string pad(indent, ' ');
	for (int i = 0; i < info.field_count; i++)
	{
		const FieldInfo& field = info.fields[i];
		const char* data = base + field.offset;
		std::cout << pad << field.name << ": ";

		if (field.type != Struct || !field.struct_info)
		{
			PrintFieldValue(std::cout, field, data);
			std::cout << std::endl;
			continue;
		}

		std::cout << std::endl;
		const StructInfo& nested = *field.struct_info;
		const bool isArray = field.element_count > 1;
		for (int e = 0; e < field.element_count; e++)
		{
			if (isArray)
				std::cout << pad << "  [" << e << "]" << std::endl;
			PrintFields(nested, data + e * nested.total_size, indent + (isArray ? 4 : 2));
		}
	}
}

void PrintWriteTime(const timespec& timestamp)
{
	std::cout << "-------------------------------------" << std::endl;
	std::cout << "last write time: " << std::asctime(std::localtime(&timestamp.tv_sec));
}

void PrintReadError(const std::string& tagName, unsigned int err)
{
	std::cout << "Read tag '" << tagName << "' failed, error code " << err << "." << std::endl;
}

bool ReadTagData(int conn, const std::string& tagName, std::vector<char>& data, timespec& timestamp)
{
	unsigned int err = 0;
	if (readb(conn, tagName.c_str(), data.data(), (int)data.size(), &err, &timestamp))
		return true;
	PrintReadError(tagName, err);
	return false;
}

void PrintSimpleTag(int conn, const std::string& tagName, int typecode, int count)
{
	const TypeInfo* type = FindTypeByCode(typecode);
	if (!type || !type->print || type->size == 0)
	{
		std::cout << "Unknown type code " << typecode << "." << std::endl;
		return;
	}

	std::vector<char> data(type->size * count);
	timespec timestamp{};
	if (!ReadTagData(conn, tagName, data, timestamp))
		return;

	std::cout << "value: ";
	PrintElements(std::cout, *type, data.data(), type->size, count);
	std::cout << std::endl;
	PrintWriteTime(timestamp);
}

void PrintStringTag(int conn, const std::string& tagName, int capacity)
{
	std::vector<char> text(capacity + 1);
	timespec timestamp{};
	unsigned int err = 0;
	if (!readb_string(conn, tagName.c_str(), text.data(), (int)text.size(), &err, &timestamp))
	{
		PrintReadError(tagName, err);
		return;
	}

	std::cout << "字符串长度:" << strlen(text.data()) << std::endl;
	std::cout << "字符串内容:" << text.data() << std::endl;
	PrintWriteTime(timestamp);
}

void PrintStructTag(int conn, const std::string& tagName, const std::string& className, int count)
{
	const StructInfo* info = FindStructByName(className);
	if (!info)
	{
		std::cout << "Custom type '" << className << "' not found in local registry." << std::endl;
		return;
	}

	std::vector<char> data(info->total_size * count);
	timespec timestamp{};
	if (!ReadTagData(conn, tagName, data, timestamp))
		return;

	for (int i = 0; i < count; i++)
	{
		if (count > 1)
			std::cout << "[" << i << "]" << std::endl;
		PrintFields(*info, data.data() + i * info->total_size, 2);
	}
	PrintWriteTime(timestamp);
}

} // namespace

void PrintTag(int conn, const std::string& tagName)
{
	char descriptor[kTypeDescriptorBufferSize];
	int descriptorSize = 0;
	unsigned int err = 0;
	if (!readtype(conn, "BOARD", tagName.c_str(), descriptor, sizeof(descriptor), &descriptorSize, &err))
	{
		std::cout << "Read type of tag '" << tagName << "' failed, error code " << err << "." << std::endl;
		return;
	}
	descriptorSize = std::min(descriptorSize, (int)sizeof(descriptor));
	if (descriptorSize < (int)sizeof(TypeDescriptorHeader))
	{
		std::cout << "Invalid type descriptor of tag '" << tagName << "'." << std::endl;
		return;
	}

	TypeDescriptorHeader header;
	memcpy(&header, descriptor, sizeof(header));
	const int count = (header.arraysize > 0) ? header.arraysize : 1;

	if (header.typecode == kStructTypeCode)
	{
		const char* name = descriptor + sizeof(header);
		const std::string className(name, strnlen(name, descriptorSize - sizeof(header)));
		PrintStructTag(conn, tagName, className, count);
	}
	else if (header.typecode == Char && header.arraysize > 0)
	{
		PrintStringTag(conn, tagName, header.arraysize);
	}
	else
	{
		PrintSimpleTag(conn, tagName, header.typecode, count);
	}
}

void PrintBoardInfo(int conn)
{
	BOARD_INFO info;
	unsigned int err = 0;
	if (!readboardinfo(conn, &info, sizeof(info), &err))
	{
		std::cout << "Failed to describe board, error code " << err << "." << std::endl;
		return;
	}
	std::cout << "Total  size: " << info.totalsize << std::endl;
	std::cout << "Remain size: " << info.remainsize << std::endl;
	std::cout << "Tag count (head):   " << info.tagcount_head << std::endl;
	std::cout << "Tag count (active): " << info.tagcount_act << std::endl;
}

void PrintTypes()
{
	std::cout << "Built-in types:" << std::endl;
	for (const TypeInfo& type : GetBuiltinTypes())
		std::cout << "  " << type.name << "  (" << type.size << " bytes)" << std::endl;

	std::cout << std::endl << "Registered struct types:" << std::endl;
	for (const auto& [name, info] : GetStructRegistry())
	{
		std::cout << "  " << name << "  (" << info->total_size << " bytes, "
			<< info->field_count << " fields)" << std::endl;
		for (int i = 0; i < info->field_count; i++)
		{
			const FieldInfo& field = info->fields[i];
			std::cout << "    +" << field.offset << "  " << field.name << "  (" << field.size << " bytes)";
			if (field.type == Struct && field.struct_info)
				std::cout << "  -> " << field.struct_info->name;
			if (field.element_count > 1)
				std::cout << "  [" << field.element_count << "]";
			std::cout << std::endl;
		}
	}
}
