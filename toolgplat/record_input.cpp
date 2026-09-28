#include "record_input.h"

#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cstring>

#include "text_util.h"

namespace
{

struct PathSegment
{
	std::string name;
	int index = -1;   // -1: 无下标
};

bool ParseSegment(const std::string& text, PathSegment& segment)
{
	const size_t open = text.find('[');
	if (open == std::string::npos)
	{
		segment = {text, -1};
		return !text.empty();
	}
	if (open == 0 || text.back() != ']')
		return false;
	segment.name = text.substr(0, open);
	return ParseInt(text.substr(open + 1, text.size() - open - 2), segment.index) && segment.index >= 0;
}

const FieldInfo* FindField(const StructInfo& info, const std::string& name)
{
	for (int i = 0; i < info.field_count; i++)
		if (name == info.fields[i].name)
			return &info.fields[i];
	return nullptr;
}

template <typename T>
bool ParseInteger(const std::string& text, char* out)
{
	T value{};
	const char* end = text.data() + text.size();
	auto [ptr, ec] = std::from_chars(text.data(), end, value);
	if (ec != std::errc() || ptr != end)
		return false;
	memcpy(out, &value, sizeof(T));
	return true;
}

template <typename T>
bool ParseFloating(const std::string& text, char* out)
{
	if (text.empty())
		return false;
	char* end = nullptr;
	errno = 0;
	const double value = std::strtod(text.c_str(), &end);
	if (*end != '\0' || errno == ERANGE)
		return false;
	const T converted = static_cast<T>(value);
	memcpy(out, &converted, sizeof(T));
	return true;
}

bool ParseBoolean(const std::string& text, char* out)
{
	const std::string lower = ToLower(text);
	bool value;
	if (lower == "true" || lower == "1")
		value = true;
	else if (lower == "false" || lower == "0")
		value = false;
	else
		return false;
	memcpy(out, &value, sizeof(value));
	return true;
}

// PodString<N>: char m_data[N+1] 位于 offset 0，size_t m_size 位于对象末尾
bool ParsePodString(const std::string& text, const FieldInfo& field, char* out, std::string& error)
{
	if ((int)text.size() > field.string_capacity)
	{
		error = "String too long for field '" + std::string(field.name) + "' (max "
			+ std::to_string(field.string_capacity) + " chars).";
		return false;
	}
	const int elementSize = field.size / field.element_count;
	const size_t length = text.size();
	memcpy(out, text.c_str(), length + 1);
	memcpy(out + elementSize - sizeof(size_t), &length, sizeof(size_t));
	return true;
}

bool ParseElement(const FieldInfo& field, const std::string& text, char* out, std::string& error)
{
	bool ok = false;
	switch (field.type)
	{
	case Boolean: ok = ParseBoolean(text, out); break;
	case Char:    ok = (text.size() == 1); if (ok) *out = text[0]; break;
	case Int16:   ok = ParseInteger<int16_t>(text, out); break;
	case UInt16:  ok = ParseInteger<uint16_t>(text, out); break;
	case Int32:   ok = ParseInteger<int32_t>(text, out); break;
	case UInt32:  ok = ParseInteger<uint32_t>(text, out); break;
	case Int64:   ok = ParseInteger<int64_t>(text, out); break;
	case UInt64:  ok = ParseInteger<uint64_t>(text, out); break;
	case Single:  ok = ParseFloating<float>(text, out); break;
	case Double:  ok = ParseFloating<double>(text, out); break;
	case String:  return ParsePodString(text, field, out, error);
	default:
		error = "Field '" + std::string(field.name) + "' cannot be assigned.";
		return false;
	}
	if (!ok)
		error = "Invalid value '" + text + "' for field '" + std::string(field.name) + "'.";
	return ok;
}

// 把 value 写入 base 所指 struct 中由 path[depth..] 指定的字段
bool Assign(const StructInfo& info, char* base, const std::vector<std::string>& path, size_t depth,
	const std::string& value, std::string& error)
{
	PathSegment segment;
	if (!ParseSegment(path[depth], segment))
	{
		error = "Invalid field path '" + path[depth] + "'.";
		return false;
	}
	const FieldInfo* field = FindField(info, segment.name);
	if (!field)
	{
		error = "Unknown field '" + segment.name + "' in " + info.name + ".";
		return false;
	}
	if (segment.index >= field->element_count)
	{
		error = "Index " + std::to_string(segment.index) + " out of range for field '" + segment.name
			+ "' (size " + std::to_string(field->element_count) + ").";
		return false;
	}

	const int elementSize = field->size / field->element_count;
	char* data = base + field->offset;
	const bool isLast = (depth + 1 == path.size());

	if (field->type == Struct)
	{
		if (isLast)
		{
			error = "Field '" + segment.name + "' is a struct, assign its members, e.g. " + segment.name + ".<member>=...";
			return false;
		}
		if (segment.index < 0 && field->element_count > 1)
		{
			error = "Field '" + segment.name + "' is an array, specify an index, e.g. " + segment.name + "[0].<member>=...";
			return false;
		}
		const int index = (segment.index < 0) ? 0 : segment.index;
		return Assign(*field->struct_info, data + index * elementSize, path, depth + 1, value, error);
	}

	if (!isLast)
	{
		error = "Field '" + segment.name + "' has no members.";
		return false;
	}
	if (segment.index >= 0)
		return ParseElement(*field, value, data + segment.index * elementSize, error);

	const std::vector<std::string> items = (field->element_count > 1)
		? SplitAndTrim(value, ',') : std::vector<std::string>{value};
	if ((int)items.size() > field->element_count)
	{
		error = "Too many values for field '" + segment.name + "' (size " + std::to_string(field->element_count) + ").";
		return false;
	}
	for (size_t i = 0; i < items.size(); i++)
		if (!ParseElement(*field, items[i], data + i * elementSize, error))
			return false;
	return true;
}

} // namespace

bool BuildRecord(const StructInfo& info, const std::vector<std::string>& assignments,
	std::vector<char>& record, std::string& error)
{
	record.assign(info.total_size, 0);
	for (const std::string& assignment : assignments)
	{
		const size_t eq = assignment.find('=');
		if (eq == std::string::npos || eq == 0)
		{
			error = "Expected <field>=<value>, got '" + assignment + "'.";
			return false;
		}
		const std::vector<std::string> path = SplitAndTrim(assignment.substr(0, eq), '.');
		if (!Assign(info, record.data(), path, 0, assignment.substr(eq + 1), error))
			return false;
	}
	return true;
}
