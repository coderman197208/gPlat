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

const TypeInfo* FindPrintableType(int typecode)
{
	const TypeInfo* type = FindTypeByCode(typecode);
	if (type && type->print && type->size > 0)
		return type;
	std::cout << "Unknown type code " << typecode << "." << std::endl;
	return nullptr;
}

const StructInfo* FindLocalStruct(const std::string& className)
{
	const StructInfo* info = FindStructByName(className);
	if (!info)
		std::cout << "Custom type '" << className << "' not found in local registry." << std::endl;
	return info;
}

void PrintSimpleValue(const TypeInfo& type, const char* data, int count)
{
	std::cout << "value: ";
	PrintElements(std::cout, type, data, type.size, count);
	std::cout << std::endl;
}

void PrintStringValue(const char* text, size_t length)
{
	std::cout << "字符串长度:" << length << std::endl;
	std::cout << "字符串内容:" << std::string(text, length) << std::endl;
}

void PrintStructValue(const StructInfo& info, const char* data, int count)
{
	for (int i = 0; i < count; i++)
	{
		if (count > 1)
			std::cout << "[" << i << "]" << std::endl;
		PrintFields(info, data + i * info.total_size, 2);
	}
}

void PrintRaw(const char* data, int size)
{
	std::cout << "raw: ";
	PrintHex(std::cout, data, size);
	std::cout << std::endl;
}

void PrintSimpleTag(int conn, const std::string& tagName, int typecode, int count)
{
	const TypeInfo* type = FindPrintableType(typecode);
	if (!type)
		return;

	std::vector<char> data(type->size * count);
	timespec timestamp{};
	if (!ReadTagData(conn, tagName, data, timestamp))
		return;

	PrintSimpleValue(*type, data.data(), count);
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

	PrintStringValue(text.data(), strlen(text.data()));
	PrintWriteTime(timestamp);
}

void PrintStructTag(int conn, const std::string& tagName, const std::string& className, int count)
{
	const StructInfo* info = FindLocalStruct(className);
	if (!info)
		return;

	std::vector<char> data(info->total_size * count);
	timespec timestamp{};
	if (!ReadTagData(conn, tagName, data, timestamp))
		return;

	PrintStructValue(*info, data.data(), count);
	PrintWriteTime(timestamp);
}

void PrintStructLayout(const StructInfo& info)
{
	for (int i = 0; i < info.field_count; i++)
	{
		const FieldInfo& field = info.fields[i];
		std::cout << "    +" << field.offset << "  " << field.name << "  (" << field.size << " bytes)";
		if (field.type == Struct && field.struct_info)
			std::cout << "  -> " << field.struct_info->name;
		if (field.element_count > 1)
			std::cout << "  [" << field.element_count << "]";
		std::cout << std::endl;
	}
}

// 字符数组不一定以 '\0' 结尾
std::string FixedString(const char* text, size_t capacity)
{
	return std::string(text, strnlen(text, capacity));
}

} // namespace

bool ReadTagType(int conn, const std::string& tagName, TypeDescriptorHeader& header, std::string& className)
{
	char descriptor[kTypeDescriptorBufferSize];
	int size = 0;
	unsigned int err = 0;
	if (!readtype(conn, "BOARD", tagName.c_str(), descriptor, sizeof(descriptor), &size, &err))
	{
		std::cout << "Read type of tag '" << tagName << "' failed, error code " << err << "." << std::endl;
		return false;
	}
	if (!ParseTypeDescriptor(descriptor, std::min(size, (int)sizeof(descriptor)), header, className))
	{
		std::cout << "Invalid type descriptor of tag '" << tagName << "'." << std::endl;
		return false;
	}
	return true;
}

void PrintTagData(const TypeDescriptorHeader& header, const std::string& className, const char* data, int size)
{
	if (header.typecode == Char && header.arraysize > 0)
	{
		PrintStringValue(data, strnlen(data, size));
		return;
	}

	const int count = (header.arraysize > 0) ? header.arraysize : 1;
	const StructInfo* info = nullptr;
	const TypeInfo* type = nullptr;
	if (header.typecode == kStructTypeCode)
		info = FindLocalStruct(className);
	else
		type = FindPrintableType(header.typecode);

	if (info)
		PrintStructValue(*info, data, count);
	else if (type)
		PrintSimpleValue(*type, data, count);
}

void PrintTag(int conn, const std::string& tagName)
{
	TypeDescriptorHeader header;
	std::string className;
	if (!ReadTagType(conn, tagName, header, className))
		return;
	const int count = (header.arraysize > 0) ? header.arraysize : 1;

	if (header.typecode == kStructTypeCode)
		PrintStructTag(conn, tagName, className, count);
	else if (header.typecode == Char && header.arraysize > 0)
		PrintStringTag(conn, tagName, header.arraysize);
	else
		PrintSimpleTag(conn, tagName, header.typecode, count);
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
		PrintStructLayout(*info);
	}
}

void PrintQueueInfo(const std::string& queueName, const QUEUE_HEAD& head, const std::string& typeName, const StructInfo* type)
{
	const bool shiftMode = head.operateMode == SHIFT_MODE;
	std::cout << "Queue:       " << queueName << std::endl;
	std::cout << "Record type: " << (typeName.empty() ? "<unknown>" : typeName) << "  (" << head.size << " bytes)" << std::endl;
	std::cout << "Mode:        " << (shiftMode ? "shift" : "normal") << std::endl;
	std::cout << "Slots:       " << head.num << std::endl;
	if (shiftMode)
	{
		std::cout << "Has data:    " << (head.readPoint != 0 ? "yes" : "no") << std::endl;
	}
	else
	{
		const int pending = head.num > 0 ? (head.writePoint - head.readPoint + head.num) % head.num : 0;
		std::cout << "Pending:     " << pending << " / " << head.num - 1 << std::endl;
	}
	std::cout << "Read point:  " << head.readPoint << std::endl;
	std::cout << "Write point: " << head.writePoint << std::endl;
	std::cout << "Created:     " << FixedString(head.createDate, sizeof(head.createDate)) << std::endl;

	if (type)
	{
		std::cout << "Fields:" << std::endl;
		PrintStructLayout(*type);
	}
	else
	{
		std::cout << "Record type is not registered locally, records are shown as hex." << std::endl;
	}
}

void PrintQueueRecord(const RECORD_HEAD& recordHead, const StructInfo* type, const char* data, int size)
{
	if (type)
		PrintFields(*type, data, 2);
	else
		PrintRaw(data, size);
	std::cout << "-------------------------------------" << std::endl;
	std::cout << "write time: " << FixedString(recordHead.createDate, sizeof(recordHead.createDate))
		<< ", from " << FixedString(recordHead.remoteIp, sizeof(recordHead.remoteIp)) << std::endl;
}
