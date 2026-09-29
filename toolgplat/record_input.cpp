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

// 按顶层逗号切分，[] 和 {} 内的逗号不切分
std::vector<std::string> SplitTopLevel(const std::string& text)
{
	std::vector<std::string> parts;
	if (Trim(text).empty())
		return parts;
	int depth = 0;
	std::string part;
	for (char c : text)
	{
		if (c == '[' || c == '{')
			depth++;
		else if ((c == ']' || c == '}') && depth > 0)
			depth--;
		if (c == ',' && depth == 0)
		{
			parts.push_back(Trim(part));
			part.clear();
		}
		else
			part += c;
	}
	parts.push_back(Trim(part));
	return parts;
}

// text 整体被一对匹配的 open/close 包裹时取出内部
bool Unwrap(const std::string& text, char open, char close, std::string& inner)
{
	const std::string t = Trim(text);
	if (t.size() < 2 || t.front() != open || t.back() != close)
		return false;
	int depth = 0;
	for (size_t i = 0; i < t.size(); i++)
	{
		if (t[i] == '[' || t[i] == '{')
			depth++;
		else if (t[i] == ']' || t[i] == '}')
			depth--;
		if (depth == 0 && i + 1 < t.size())
			return false;
	}
	inner = t.substr(1, t.size() - 2);
	return true;
}

bool AssignField(const FieldInfo& field, char* data, const std::string& text, std::string& error);

// 单个元素：基本类型直接解析，struct 用 {v1,v2,...} 按成员顺序赋值
bool AssignElement(const FieldInfo& field, char* out, const std::string& text, std::string& error)
{
	if (field.type != Struct)
		return ParseElement(field, text, out, error);

	std::string inner;
	if (!Unwrap(text, '{', '}', inner))
	{
		error = "Field '" + std::string(field.name) + "' is a struct, use {v1,v2,...} or assign members, e.g. "
			+ field.name + ".<member>=...";
		return false;
	}
	const StructInfo& info = *field.struct_info;
	const std::vector<std::string> items = SplitTopLevel(inner);
	if ((int)items.size() > info.field_count)
	{
		error = "Too many values for struct field '" + std::string(field.name) + "' (" + info.name + " has "
			+ std::to_string(info.field_count) + " members).";
		return false;
	}
	for (size_t i = 0; i < items.size(); i++)
		if (!AssignField(info.fields[i], out + info.fields[i].offset, items[i], error))
			return false;
	return true;
}

// 整个字段：数组用 [v0,v1,...]（也接受不带括号的逗号列表）
bool AssignField(const FieldInfo& field, char* data, const std::string& text, std::string& error)
{
	if (field.element_count == 1)
		return AssignElement(field, data, text, error);

	std::string list;
	if (!Unwrap(text, '[', ']', list))
	{
		list = Trim(text);
		if (!list.empty() && (list.front() == '[' || list.back() == ']'))
		{
			error = "Unbalanced brackets in value for field '" + std::string(field.name) + "'.";
			return false;
		}
	}
	const std::vector<std::string> items = SplitTopLevel(list);
	if ((int)items.size() > field.element_count)
	{
		error = "Too many values for field '" + std::string(field.name) + "' (size "
			+ std::to_string(field.element_count) + ").";
		return false;
	}
	const int elementSize = field.size / field.element_count;
	for (size_t i = 0; i < items.size(); i++)
		if (!AssignElement(field, data + i * elementSize, items[i], error))
			return false;
	return true;
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

	if (field->type == Struct && !isLast)
	{
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
		return AssignElement(*field, data + segment.index * elementSize, value, error);
	return AssignField(*field, data, value, error);
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

bool ApplyTagValues(const FieldInfo& tag, char* data, const std::vector<std::string>& words, std::string& error)
{
	const StructInfo wrapper{tag.name, tag.size, 1, &tag};
	for (const std::string& word : words)
	{
		const size_t eq = word.find('=');
		const bool isAssignment = eq != std::string::npos && eq > 0 && (word[0] == '[' || tag.type == Struct);
		if (!isAssignment)
		{
			if (words.size() != 1)
			{
				error = "Expected a single value or <path>=<value> items, got '" + word + "'.";
				return false;
			}
			return AssignField(tag, data, word, error);
		}

		// 路径首段固定为 tag 本身，tag 名不参与 '.' 切分
		const std::string pathText = Trim(word.substr(0, eq));
		std::string first = tag.name;
		std::string rest = pathText;
		if (pathText[0] == '[')
		{
			const size_t close = pathText.find(']');
			if (close == std::string::npos)
			{
				error = "Invalid path '" + pathText + "'.";
				return false;
			}
			first += pathText.substr(0, close + 1);
			rest = pathText.substr(close + 1);
			if (!rest.empty() && rest[0] != '.')
			{
				error = "Invalid path '" + pathText + "'.";
				return false;
			}
			if (!rest.empty())
				rest.erase(0, 1);
		}
		std::vector<std::string> path{first};
		if (!rest.empty())
		{
			const std::vector<std::string> members = SplitAndTrim(rest, '.');
			path.insert(path.end(), members.begin(), members.end());
		}
		if (!Assign(wrapper, data, path, 0, word.substr(eq + 1), error))
			return false;
	}
	return true;
}
