#ifndef TEXT_UTIL_H_
#define TEXT_UTIL_H_

#include <algorithm>
#include <cctype>
#include <charconv>
#include <sstream>
#include <string>
#include <vector>

inline std::string ToLower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
	return s;
}

inline std::string ToUpper(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
	return s;
}

inline std::string Trim(const std::string& s)
{
	const char* whitespace = " \t\r\n";
	size_t begin = s.find_first_not_of(whitespace);
	if (begin == std::string::npos)
		return "";
	size_t end = s.find_last_not_of(whitespace);
	return s.substr(begin, end - begin + 1);
}

// 去掉 '#' 起的注释及首尾空白
inline std::string StripComment(const std::string& line)
{
	return Trim(line.substr(0, line.find('#')));
}

// 按空白切分，忽略空段；双引号内的空白不切分，引号本身被去掉
inline std::vector<std::string> SplitWords(const std::string& s)
{
	std::vector<std::string> words;
	std::string word;
	bool inWord = false;
	bool quoted = false;
	for (char c : s)
	{
		if (c == '"')
		{
			quoted = !quoted;
			inWord = true;
		}
		else if (!quoted && std::isspace((unsigned char)c))
		{
			if (inWord)
				words.push_back(word);
			word.clear();
			inWord = false;
		}
		else
		{
			word += c;
			inWord = true;
		}
	}
	if (inWord)
		words.push_back(word);
	return words;
}

// 按分隔符切分，每段去首尾空白（保留空段）
inline std::vector<std::string> SplitAndTrim(const std::string& s, char delim)
{
	std::vector<std::string> parts;
	std::istringstream iss(s);
	std::string part;
	while (std::getline(iss, part, delim))
		parts.push_back(Trim(part));
	return parts;
}

// 整个字符串必须是合法整数
inline bool ParseInt(const std::string& s, int& value)
{
	const char* end = s.data() + s.size();
	auto [ptr, ec] = std::from_chars(s.data(), end, value);
	return ec == std::errc() && ptr == end;
}

#endif // TEXT_UTIL_H_
