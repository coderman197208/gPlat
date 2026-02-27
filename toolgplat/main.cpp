#include <cstdio>
// This must come first!
// 这是readline库头文件的问题。这个问题在CentOS/RedHat系统上比较常见，主要原因是C++编译器对C代码的兼容性问题。
// 如果没有#include <cstdio>，readline库中的某些函数声明可能会因为缺少C标准库的头文件而导致编译错误。
// 通过先包含<cstdio>，可以确保C标准库的函数声明被正确引入，从而解决编译问题。
#include <readline/readline.h>
#include <readline/history.h>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cstdlib> // for strtol
#include <fstream>
#include <unistd.h>
#include <string.h>
#include "..//include//higplat.h"

//-1代表复合类型
enum TypeCode
{
	Empty = 0,
	Boolean,
	Char,
	Int16,
	UInt16,
	Int32,
	UInt32,
	Int64,
	UInt64,
	Single,
	Double,
};

int GetSizeOf(TypeCode typecode)
{
	switch (typecode)
	{
	case TypeCode::Boolean:
		return sizeof(bool);
	case TypeCode::Char:
		return sizeof(char);
	case TypeCode::Int16:
		return sizeof(int16_t);
	case TypeCode::UInt16:
		return sizeof(uint16_t);
	case TypeCode::Int32:
		return sizeof(int32_t);
	case TypeCode::UInt32:
		return sizeof(uint32_t);
	case TypeCode::Int64:
		return sizeof(int64_t);
	case TypeCode::UInt64:
		return sizeof(uint64_t);
	case TypeCode::Single:
		return sizeof(float);
	case TypeCode::Double:
		return sizeof(double);
	default:
		return 0; // Empty或未知类型
	}
}

enum QBDType
{
	queue = 0,
	board,
	database
};
QBDType g_qbdtype;

int g_hConn;

struct GlobalObj
{
	static std::string prefix;
	static std::string nodename;
	static std::string qbdname;
	// Note: rectype and recFields are .NET specific and don't have direct equivalents
	// They would need to be replaced with appropriate C++ alternatives
};

std::string GlobalObj::prefix = "gplat>";
std::string GlobalObj::nodename = "";
std::string GlobalObj::qbdname = "";

void Analyse(const std::vector<std::string>& words);

int main(int argc, char* argv[])
{
	// 设置 readline 的自动补全（可选）
	rl_bind_key('\t', rl_complete); // 启用 Tab 补全

	char* line;
	std::vector<std::string> words;

	while ((line = readline(GlobalObj::prefix.c_str())))
	{ // 使用 readline 替代 std::getline
		if (!line)
			break; // 处理 Ctrl+D

		// 添加到历史记录（非空行）
		if (*line)
			add_history(line);

		// 转换为 std::string 方便处理
		std::string input(line);
		free(line); // readline 返回的指针需要手动释放

		if (input.empty())
			continue;

		// 分割单词逻辑（保持不变）
		size_t start = 0, end = 0;
		while ((end = input.find(' ', start)) != std::string::npos)
		{
			if (end != start)
			{
				words.push_back(input.substr(start, end - start));
			}
			start = end + 1;
		}
		if (start < input.length())
		{
			words.push_back(input.substr(start));
		}

		if (!words.empty())
		{
			std::string firstWord = words[0];
			std::transform(firstWord.begin(), firstWord.end(), firstWord.begin(),
				[](unsigned char c)
				{ return std::tolower(c); });

			if (firstWord == "exit" || firstWord == "q")
				break;

			Analyse(words);
		}

		words.clear();
	}

	if (g_hConn > 0)
		close(g_hConn);

	return 0;
}

void HandleConnect(const std::string& remotehost)
{
	int hConn = connectgplat(remotehost.c_str(), 8777);

	if (hConn > 0)
	{
		if (g_hConn > 0)
		{
			close(g_hConn);
		}
		g_hConn = hConn;

		GlobalObj::nodename = remotehost;
		GlobalObj::prefix = remotehost + ">";
	}
	else
	{
		std::cout << "无法连接到" << remotehost << "." << std::endl;
	}
}

void HandleOpenBoard()
{
	g_qbdtype = board;
	GlobalObj::qbdname = "BOARD";

	GlobalObj::prefix = GlobalObj::nodename + ".BOARD>";
}

// para  复合类型名$负荷类型大小 或者 简单类型名
// para2 数组大小
bool CreateItem(const std::string& itemName, const std::string& para, const std::string& para2)
{
	int arraysize = 0;

	if (!para2.empty())
	{
		try
		{
			arraysize = std::stoi(para2);
		}
		catch (...)
		{
			arraysize = 0;
		}
	}

	// Split para by '$'
	std::vector<std::string> split;
	size_t start = 0, end = para.find('$');
	while (end != std::string::npos)
	{
		split.push_back(para.substr(start, end - start));
		start = end + 1;
		end = para.find('$', start);
	}
	split.push_back(para.substr(start));

	if (split.size() > 1)
	{
		if (arraysize == 0 || arraysize == 1)
		{
			// 复合类型 - 需要实现
			char buff[2048];
			int* ptypecode = (int*)buff;
			int* parraysize = (int*)(buff + 4);
			*ptypecode = -1;
			*parraysize = 0;
			int itemsize = std::stoi(split[1]);
			char* classname = buff + 8;
			strcpy(classname, split[0].c_str());
			int typesize = 8 + split[0].length() + 1; // 类型代码，数组大小，类型名，0结束符

			// 检查大小限制
			if (itemsize > 16000)
			{
				std::cout << "TAG的大小超过了16000，无法创建" << std::endl;
				return false;
			}

			unsigned int err;
			bool res = createtag(g_hConn, itemName.c_str(), itemsize, buff, typesize, &err);

			if (res)
			{
				std::cout << "Tag '" << itemName << "' created successfully, Record size="
					<< itemsize << ", Type size=" << typesize << std::endl;
			}
			else
			{
				std::cout << "Create Tag '" << itemName << "' fail with error code " << err << std::endl;
			}

			return res;
		}
		else if (arraysize > 1)
		{
			// 复合类型数组 - 需要实现
			char buff[100];
			int* ptypecode = (int*)buff;
			int* parraysize = (int*)(buff + 4);
			*ptypecode = -1;
			*parraysize = 0;
			int itemsize = std::stoi(split[1]) * arraysize;
			char* classname = buff + 8;
			strcpy(classname, split[0].c_str());
			int typesize = 8 + split[0].length() + 1; // 类型代码，数组大小，类型名，0结束符

			// 检查大小限制
			if (itemsize > 16000)
			{
				std::cout << "TAG的大小超过了16000，无法创建" << std::endl;
				return false;
			}

			unsigned int err;
			bool res = createtag(g_hConn, itemName.c_str(), itemsize, buff, typesize, &err);

			if (res)
			{
				std::cout << "Tag '" << itemName << "' created successfully, Record size="
					<< itemsize << ", Type size=" << typesize << std::endl;
			}
			else
			{
				std::cout << "Create Tag '" << itemName << "' fail with error code " << err << std::endl;
			}

			return res;
		}
	}
	else if (arraysize == 0 || arraysize == 1)
	{
		// 简单类型
		std::string typeName = para;
		TypeCode typecode = Empty;
		size_t itemsize = 0;

		// 确定类型和大小
		if (typeName == "Boolean")
		{
			typecode = Boolean;
			itemsize = sizeof(bool);
		}
		else if (typeName == "Char")
		{
			typecode = Char;
			itemsize = sizeof(char);
		}
		else if (typeName == "Double")
		{
			typecode = Double;
			itemsize = sizeof(double);
		}
		else if (typeName == "Int16")
		{
			typecode = Int16;
			itemsize = sizeof(int16_t);
		}
		else if (typeName == "Int32")
		{
			typecode = Int32;
			itemsize = sizeof(int32_t);
		}
		else if (typeName == "Int64")
		{
			typecode = Int64;
			itemsize = sizeof(int64_t);
		}
		else if (typeName == "Single")
		{
			typecode = Single;
			itemsize = sizeof(float);
		}
		else if (typeName == "UInt16")
		{
			typecode = UInt16;
			itemsize = sizeof(uint16_t);
		}
		else if (typeName == "UInt32")
		{
			typecode = UInt32;
			itemsize = sizeof(uint32_t);
		}
		else if (typeName == "UInt64")
		{
			typecode = UInt64;
			itemsize = sizeof(uint64_t);
		}
		else
		{
			std::cout << "Type definition error!" << std::endl;
			return false;
		}

		// 检查大小限制
		if (itemsize > 16000)
		{
			std::cout << "TAG的大小超过了16000，无法创建" << std::endl;
			return false;
		}

		char buff[8];
		int* ptypecode = (int*)buff;
		int* parraysize = (int*)(buff + 4);
		*ptypecode = (int)typecode;
		*parraysize = 0;

		unsigned int err;
		bool res = createtag(g_hConn, itemName.c_str(), (int)itemsize, buff, sizeof(buff), &err);

		if (res)
		{
			std::cout << "Tag '" << itemName << "' created successfully, Record size="
				<< itemsize << ", Type size=" << sizeof(typecode) << std::endl;
		}
		else
		{
			std::cout << "Create Tag '" << itemName << "' fail with error code " << err << std::endl;
		}
		return res;
	}
	else if (arraysize > 1)
	{
		// 简单类型数组
		std::string typeName = para;
		TypeCode typecode = Empty;
		size_t itemsize = 0;

		if (typeName == "String" || typeName == "string" || typeName == "STRING")
		{
			typecode = Char;
			itemsize = sizeof(char) * arraysize;
		}
		else if (typeName == "Boolean")
		{
			typecode = Boolean;
			itemsize = sizeof(bool) * arraysize;
		}
		else if (typeName == "Char")
		{
			typecode = Char;
			itemsize = sizeof(char) * arraysize;
		}
		else if (typeName == "Double")
		{
			typecode = Double;
			itemsize = sizeof(double) * arraysize;
		}
		else if (typeName == "Int16")
		{
			typecode = Int16;
			itemsize = sizeof(int16_t) * arraysize;
		}
		else if (typeName == "Int32")
		{
			typecode = Int32;
			itemsize = sizeof(int32_t) * arraysize;
		}
		else if (typeName == "Int64")
		{
			typecode = Int64;
			itemsize = sizeof(int64_t) * arraysize;
		}
		else if (typeName == "Single")
		{
			typecode = Single;
			itemsize = sizeof(float) * arraysize;
		}
		else if (typeName == "UInt16")
		{
			typecode = UInt16;
			itemsize = sizeof(uint16_t) * arraysize;
		}
		else if (typeName == "UInt32")
		{
			typecode = UInt32;
			itemsize = sizeof(uint32_t) * arraysize;
		}
		else if (typeName == "UInt64")
		{
			typecode = UInt64;
			itemsize = sizeof(uint64_t) * arraysize;
		}
		else
		{
			std::cout << "Type definition error!" << std::endl;
			return false;
		}

		// 检查大小限制
		if (itemsize > 16000)
		{
			std::cout << "TAG的大小超过了16000，无法创建" << std::endl;
			return false;
		}

		char buff[8];
		int* ptypecode = (int*)buff;
		int* parraysize = (int*)(buff + 4);
		*ptypecode = (int)typecode;
		*parraysize = arraysize;

		unsigned int err;
		bool res = createtag(g_hConn, itemName.c_str(), (int)itemsize, buff, sizeof(buff), &err);

		if (res)
		{
			std::cout << "Array Tag '" << itemName << "' created successfully, Record size="
				<< itemsize << ", Type size=" << sizeof(buff) << std::endl;
		}
		else
		{
			std::cout << "Create Array Tag '" << itemName << "' fail with error code " << err << std::endl;
		}
		return res;
	}

	return true;
}

void CreateItemFromScriptFile(std::string fileName)
{
	std::ifstream file(fileName);
	if (!file.is_open())
	{
		std::cout << "无法打开文件: " << fileName << std::endl;
		return;
	}

	std::string line;
	int successCount = 0;
	int failCount = 0;

	while (std::getline(file, line))
	{
		// 去除首尾空白
		size_t s = line.find_first_not_of(" \t\r\n");
		if (s == std::string::npos)
			continue;
		line = line.substr(s);
		size_t e = line.find_last_not_of(" \t\r\n");
		if (e != std::string::npos)
			line = line.substr(0, e + 1);
		// 跳过注释行
		if (line[0] == '#' || line[0] == ';')
			continue;
		// 按空格分割
		std::vector<std::string> parts;
		size_t pos = 0;
		while ((pos = line.find(' ')) != std::string::npos)
		{
			std::string part = line.substr(0, pos);
			if (!part.empty())
				parts.push_back(part);
			line.erase(0, pos + 1);
		}
		if (!line.empty())
			parts.push_back(line);
		if (parts.size() >= 3 && parts[0] == "create")
		{
			std::string itemName = parts[1];
			std::string typeName = parts[2];
			std::string arraySize = (parts.size() >= 4) ? parts[3] : "";
			if (CreateItem(itemName, typeName, arraySize))
				successCount++;
			else
				failCount++;
		}
	}

	std::cout << "CreateItemFromScriptFile 完成: 成功 " << successCount << " 个, 失败 " << failCount << " 个" << std::endl;
}

void CreateItemFromConfigFile(std::string fileName)
{
	std::ifstream file(fileName);
	if (!file.is_open())
	{
		std::cout << "无法打开文件: " << fileName << std::endl;
		return;
	}

	// S7数据类型 -> CreateItem类型名 的映射
	auto mapType = [](const std::string& s7type) -> std::string
		{
			if (s7type == "BOOL")
				return "Boolean";
			if (s7type == "INT")
				return "Int16";
			if (s7type == "DINT")
				return "Int32";
			if (s7type == "WORD")
				return "UInt16";
			if (s7type == "DWORD")
				return "UInt32";
			if (s7type == "REAL")
				return "Single";
			if (s7type == "STRING")
				return "Char";
			return "";
		};

	bool inPlcSection = false;
	std::string line;
	int successCount = 0;
	int failCount = 0;

	while (std::getline(file, line))
	{
		// 去除首尾空白
		size_t s = line.find_first_not_of(" \t\r\n");
		if (s == std::string::npos)
			continue;
		line = line.substr(s);
		size_t e = line.find_last_not_of(" \t\r\n");
		if (e != std::string::npos)
			line = line.substr(0, e + 1);

		// 跳过注释行
		if (line[0] == '#' || line[0] == ';')
			continue;

		// 处理 section 头
		if (line[0] == '[')
		{
			size_t end = line.find(']');
			if (end != std::string::npos)
			{
				std::string section = line.substr(1, end - 1);
				inPlcSection = (section != "general");
			}
			continue;
		}

		if (!inPlcSection)
			continue;

		// 解析 key = value
		size_t eqPos = line.find('=');
		if (eqPos == std::string::npos)
			continue;

		std::string key = line.substr(0, eqPos);
		std::string value = line.substr(eqPos + 1);

		// 去除 key 首尾空白
		size_t ks = key.find_first_not_of(" \t");
		size_t ke = key.find_last_not_of(" \t");
		if (ks == std::string::npos)
			continue;
		key = key.substr(ks, ke - ks + 1);

		// 去除 value 首尾空白
		size_t vs = value.find_first_not_of(" \t");
		if (vs == std::string::npos)
			continue;
		value = value.substr(vs);

		// 跳过PLC的连接参数行
		if (key == "ip" || key == "rack" || key == "slot" || key == "poll_interval")
			continue;

		// 按逗号分割: area, db号, 偏移, 数据类型 [, 最大长度]
		std::vector<std::string> parts;
		size_t pos = 0;
		while (pos <= value.size())
		{
			size_t commaPos = value.find(',', pos);
			std::string part;
			if (commaPos == std::string::npos)
			{
				part = value.substr(pos);
				pos = value.size() + 1;
			}
			else
			{
				part = value.substr(pos, commaPos - pos);
				pos = commaPos + 1;
			}
			size_t ps = part.find_first_not_of(" \t");
			size_t pe = part.find_last_not_of(" \t");
			if (ps != std::string::npos)
				parts.push_back(part.substr(ps, pe - ps + 1));
			else
				parts.push_back("");
		}

		// 至少需要: 区域, DB号, 偏移, 数据类型
		if (parts.size() < 4)
			continue;

		std::string s7type = parts[3];
		std::transform(s7type.begin(), s7type.end(), s7type.begin(),
			[](unsigned char c)
			{ return std::toupper(c); });

		std::string createType = mapType(s7type);
		if (createType.empty())
		{
			std::cout << "未知数据类型 '" << s7type << "' (tag: " << key << ")，跳过" << std::endl;
			continue;
		}

		// STRING类型：第三个参数为最大长度，其余为 ""
		std::string maxLen = "";
		if (s7type == "STRING" && parts.size() >= 5 && !parts[4].empty())
		{
			if (parts.size() >= 5 && !parts[4].empty())
			{
				maxLen = parts[4];
			}
			else
			{
				std::cout << "STRING类型 '" << key << "' 缺少最大长度参数，跳过" << std::endl;
				continue;
			}
		}

		if (CreateItem(key, createType, maxLen))
			successCount++;
		else
			failCount++;
	}

	std::cout << "CreateItemFromConfigFile 完成: 成功 " << successCount << " 个, 失败 " << failCount << " 个" << std::endl;
}

void HandleClearBoard(std::string qbdName)
{
	unsigned int error;

	if (qbdName != "BOARD")
	{
		std::cout << "Only BOARD can be cleared." << std::endl;
		return;
	}

	if (g_qbdtype != board) return;

	if (clearb(g_hConn, &error))
	{
		std::cout << "Board: " << qbdName << " cleared." << std::endl;
	}
}

void HandleCreate(const std::vector<std::string>& words)
{
	if (GlobalObj::qbdname.empty())
	{
		std::cout << "No db or board opened." << std::endl;
		return;
	}

	if (g_qbdtype != board)
	{
		std::cout << "Create command is only supported for board." << std::endl;
		return;
	}

	if (words.size() == 6)
	{
		std::string firstWord = words[3];
		std::transform(firstWord.begin(), firstWord.end(), firstWord.begin(),
			[](unsigned char c)
			{ return std::tolower(c); });

		if (firstWord == "config")
		{
			CreateItemFromConfigFile(words[5]);
		}
		else if (firstWord == "script")
		{
			CreateItemFromScriptFile(words[5]);
		}
	}
	else if (words.size() == 3)
	{
		CreateItem(words[1], words[2], "");
	}
	else if (words.size() == 4)
	{
		CreateItem(words[1], words[2], words[3]);
	}
}

void DeleteItem(std::string itemName)
{
	unsigned int err;

	if (deletetag(g_hConn, itemName.c_str(), &err))
	{
		std::cout << "Tag " << itemName <<" deleted" << std::endl;
	}
}

void SelectItem(std::string itemName)
{
	// 获取记录类型
	int typesize;
	unsigned int err;

	char buffer[2048];

	if (readtype(g_hConn, "BOARD", itemName.c_str(), buffer, 2048, &typesize, &err))
	{
		char* buff = (char*)buffer;
		int typecode = *(int*)buff;
		int* parraysize = (int*)(buff + 4);
		int arraysize = *parraysize;

		if (typecode > 0)
		{
			// 简单类型或简单数组类型
			TypeCode* ptypecode = (TypeCode*)buff;
			TypeCode typecode = *ptypecode;

			if (arraysize == 0)
			{
				// 简单类型
				int itemsize = GetSizeOf(typecode);
				char value[128];
				timespec timestamp;

				if (readb(g_hConn, itemName.c_str(), value, itemsize, &err, &timestamp))
				{
					std::cout << "value: ";
					// 根据typecode输出value的值
					switch (typecode)
					{
					case Boolean:
						std::cout << (value[0] != 0 ? "true" : "false") << std::endl;
						break;
					case Char:
						std::cout << value[0] << std::endl;
						break;
					case Int16:
						std::cout << *(int16_t*)value << std::endl;
						break;
					case UInt16:
						std::cout << *(uint16_t*)value << std::endl;
						break;
					case Int32:
						std::cout << *(int32_t*)value << std::endl;
						break;
					case UInt32:
						std::cout << *(uint32_t*)value << std::endl;
						break;
					case Int64:
						std::cout << *(int64_t*)value << std::endl;
						break;
					case UInt64:
						std::cout << *(uint64_t*)value << std::endl;
						break;
					case Single:
						std::cout << *(float*)value << std::endl;
						break;
					case Double:
						std::cout << *(double*)value << std::endl;
						break;
					default:
						std::cout << "Unknown type code." << std::endl;
						break;
					}

					std::cout << "-------------------------------------" << std::endl;
					std::cout << "last write time: " << std::asctime(std::localtime(&timestamp.tv_sec));
				}
			}
			else
			{
				// 简单类型数组
				timespec timestamp;

				if (typecode == TypeCode::Char)
				{
					char value[4096];
					if (readb_string(g_hConn, itemName.c_str(), value, 4096, &err, &timestamp))
					{
						std::cout << "字符串长度:" << strlen(value) << std::endl;
						std::cout << "字符串内容:" << value << std::endl;
						std::cout << "-------------------------------------" << std::endl;
						std::cout << "last write time: " << std::asctime(std::localtime(&timestamp.tv_sec));
					}
				}
			}
		}
	}
}

void HandleSelect(std::string itemName)
{
	if (GlobalObj::qbdname.empty())
	{
		std::cout << "No db or board opened." << std::endl;
		return;
	}

	if (g_qbdtype == database)
	{
		//SelectTable(itemName);
	}
	else if (g_qbdtype == board)
	{
		SelectItem(itemName);
	}
}

void HandleDelete(std::string itemName)
{
	if (GlobalObj::qbdname.empty())
	{
		std::cout << "No db or board opened." << std::endl;
		return;
	}

	if (g_qbdtype == database)
	{
		//DeleteTable(itemName);
	}
	else if (g_qbdtype == board)
	{
		DeleteItem(itemName);
	}
}

void Analyse(const std::vector<std::string>& words)
{
	if (words.empty())
		return;

	std::string cmd = words[0];
	// 转换为小写
	std::transform(cmd.begin(), cmd.end(), cmd.begin(),
		[](unsigned char c)
		{ return std::tolower(c); });

	if (cmd == "clear")
	{
		if (words.size() > 1)
		{
			if (g_qbdtype == database)
			{
				//HandleClearDB(GlobalObj::qbdname);
			}
			else if (g_qbdtype == board)
			{
				HandleClearBoard(GlobalObj::qbdname);
			}
		}
		return;
	}

	if (cmd == "conn" || cmd == "connect")
	{
		if (words.size() < 2)
		{
			HandleConnect("127.0.0.1");
		}
		else
		{
			HandleConnect(words[1]);
		}
		return;
	}

	if (cmd == "openb")
	{
		HandleOpenBoard();
		return;
	}

	//if (cmd == "opendb")
	//{
	//   HandleOpenDB(words[1]);
	//   return;
	//}

	if (cmd == "select")
	{
		HandleSelect(words[1]);
		return;
	}

	if (cmd == "create")
	{
		HandleCreate(words);
		return;
	}

	if (cmd == "delete")
	{
		HandleDelete(words[1]);
		return;
	}

	std::cout << "Unknown command." << std::endl;
}
