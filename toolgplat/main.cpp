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
#include <iomanip>
#include <fstream>
#include <unistd.h>
#include <string.h>

#include "..//include//higplat.h"
#include "type_handle.h"

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

// para  简单类型名 或者 自定义类型名 或者 自定义类型名$自定义类型大小
// para2 数组大小
bool CreateTag(const std::string& tagName, const std::string& para, const std::string& para2)
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
		// 自定义类型（已指定大小）
		size_t itemsize = std::stoi(split[1]);
		size_t tagsize = (arraysize > 1) ? (size_t)itemsize * arraysize : (size_t)itemsize;

		// 检查大小限制
		if (itemsize > 16000)
		{
			std::cout << "TAG的大小超过了16000，无法创建" << std::endl;
			return false;
		}

		char buff[128];
		int* ptypecode = (int*)buff;
		int* parraysize = (int*)(buff + 4);
		*ptypecode = -1;
		*parraysize = (arraysize > 1) ? arraysize : 0;;

		char* classname = buff + 8;
		strcpy(classname, split[0].c_str());
		int typesize = 8 + split[0].length() + 1; // 类型代码，数组大小，类型名，0结束符

		unsigned int err;
		bool res = createtag(g_hConn, tagName.c_str(), tagsize, buff, typesize, &err);

		if (res)
		{
			std::cout << "Tag '" << tagName << "' created successfully, Record size="
				<< itemsize << ", Type size=" << typesize << std::endl;
		}
		else
		{
			std::cout << "Create Tag '" << tagName << "' fail with error code " << err << std::endl;
		}

		return res;
	}
	else
	{
		// 简单类型（标量或数组）
		std::string typeName = para;

		// String/string/STRING 当作 Char 处理
		if (typeName == "String" || typeName == "string" || typeName == "STRING")
			typeName = "Char";

		const TypeInfo* ti = FindTypeByName(typeName);
		if (!ti)
		{
			// 查找已注册的自定义 struct
			const StructInfo* si = FindStructByName(typeName);
			if (si)
			{
				size_t itemsize = si->total_size;
				size_t tagsize = (arraysize > 1) ? itemsize * arraysize : itemsize;

				if (tagsize > 16000)
				{
					std::cout << "TAG的大小超过了16000，无法创建" << std::endl;
					return false;
				}

				// 构建类型元数据 buffer: [typecode=-1][arraysize][classname\0]
				char buff[128];
				int* ptypecode = (int*)buff;
				int* parraysize_p = (int*)(buff + 4);
				*ptypecode = -1;
				*parraysize_p = (arraysize > 1) ? arraysize : 0;
				char* classname = buff + 8;
				strcpy(classname, typeName.c_str());
				int typesize = 8 + typeName.length() + 1;

				unsigned int err;
				bool res = createtag(g_hConn, tagName.c_str(), tagsize, buff, typesize, &err);

				if (res)
					std::cout << "Tag '" << tagName << "' created (struct " << typeName
						<< ", " << itemsize << " bytes)" << std::endl;
				else
					std::cout << "Create Tag '" << tagName << "' fail with error code " << err << std::endl;

				return res;
			}

			std::cout << "Type definition error!" << std::endl;
			return false;
		}

		size_t tagsize = (arraysize > 1) ? (size_t)ti->size * arraysize : (size_t)ti->size;

		// 检查大小限制
		if (tagsize > 16000)
		{
			std::cout << "TAG的大小超过了16000，无法创建" << std::endl;
			return false;
		}

		char buff[8];
		int* ptypecode = (int*)buff;
		int* parraysize_p = (int*)(buff + 4);
		*ptypecode = (int)ti->code;
		*parraysize_p = (arraysize > 1) ? arraysize : 0;

		unsigned int err;
		bool res = createtag(g_hConn, tagName.c_str(), (int)tagsize, buff, sizeof(buff), &err);

		if (res)
		{
			if (arraysize > 1)
				std::cout << "Array Tag '" << tagName << "' created successfully, Record size="
					<< tagsize << ", Type size=" << sizeof(buff) << std::endl;
			else
				std::cout << "Tag '" << tagName << "' created successfully, Record size="
					<< tagsize << ", Type size=" << sizeof(buff) << std::endl;
		}
		else
		{
			std::cout << "Create Tag '" << tagName << "' fail with error code " << err << std::endl;
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
			if (CreateTag(itemName, typeName, arraySize))
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

		std::string createType = MapS7Type(s7type);
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

		if (CreateTag(key, createType, maxLen))
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
		CreateTag(words[1], words[2], "");
	}
	else if (words.size() == 4)
	{
		CreateTag(words[1], words[2], words[3]);
	}
}

void DeleteTag(std::string itemName)
{
	unsigned int err;

	if (deletetag(g_hConn, itemName.c_str(), &err))
	{
		std::cout << "Tag " << itemName <<" deleted" << std::endl;
	}
}

inline void PrintHex(std::ostream& os, const void* data, int size)
{
	const unsigned char* p = (const unsigned char*)data;
	for (int i = 0; i < size && i < 64; i++)
		os << std::hex << std::setw(2) << std::setfill('0') << (int)p[i] << " ";
	os << std::dec;
}

void SelectTag(std::string tagName)
{
	// 获取记录类型
	int typesize;
	unsigned int err;

	char buffer[2048];

	if (readtype(g_hConn, "BOARD", tagName.c_str(), buffer, 2048, &typesize, &err))
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
				const TypeInfo* ti = FindTypeByCode((int)typecode);
				if (!ti)
				{
					std::cout << "Unknown type code." << std::endl;
					return;
				}

				int itemsize = ti->size;
				char value[128];
				timespec timestamp;

				if (readb(g_hConn, tagName.c_str(), value, itemsize, &err, &timestamp))
				{
					std::cout << "value: ";
					ti->print(std::cout, value);
					std::cout << std::endl;

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
					if (readb_string(g_hConn, tagName.c_str(), value, 4096, &err, &timestamp))
					{
						std::cout << "字符串长度:" << strlen(value) << std::endl;
						std::cout << "字符串内容:" << value << std::endl;
						std::cout << "-------------------------------------" << std::endl;
						std::cout << "last write time: " << std::asctime(std::localtime(&timestamp.tv_sec));
					}
				}
			}
		}
		else if (typecode == -1)
		{
			// 自定义 struct 类型
			const char* classname = buffer + 8;
			const StructInfo* si = FindStructByName(classname);

			if (!si)
			{
				std::cout << "Custom type '" << classname
					<< "' not found in local registry." << std::endl;
				return;
			}

			int itemcount = (arraysize > 0) ? arraysize : 1;
			int totalsize = si->total_size * itemcount;
			std::vector<char> data(totalsize);
			timespec timestamp;

			if (readb(g_hConn, tagName.c_str(), data.data(), totalsize, &err, &timestamp))
			{
				for (int idx = 0; idx < itemcount; idx++)
				{
					if (itemcount > 1)
						std::cout << "[" << idx << "]" << std::endl;

					char* base = data.data() + idx * si->total_size;

					for (int f = 0; f < si->field_count; f++)
					{
						const FieldInfo& fi = si->fields[f];
						std::cout << "  " << fi.name << ": ";

						if (fi.type == Struct && fi.struct_info)
						{
							// 嵌套 struct（限一层）
							int elem_size = fi.struct_info->total_size;
							std::cout << std::endl;
							for (int a = 0; a < fi.element_count; a++)
							{
								if (fi.element_count > 1)
									std::cout << "    [" << a << "]" << std::endl;
								char* sbase = base + fi.offset + a * elem_size;
								for (int sf = 0; sf < fi.struct_info->field_count; sf++)
								{
									const FieldInfo& sfi = fi.struct_info->fields[sf];
									const char* indent = (fi.element_count > 1) ? "      " : "    ";
									std::cout << indent << sfi.name << ": ";
									const TypeInfo* sti = FindTypeByCode((int)sfi.type);
									if (sti && sti->print)
									{
										int sa_count = sfi.element_count;
										int sa_size = (sa_count > 1) ? (sfi.size / sa_count) : sfi.size;
										if (sa_count > 1)
										{
											std::cout << "[";
											for (int sa = 0; sa < sa_count; sa++)
											{
												if (sa > 0) std::cout << ", ";
												sti->print(std::cout, sbase + sfi.offset + sa * sa_size);
											}
											std::cout << "]";
										}
										else
											sti->print(std::cout, sbase + sfi.offset);
									}
									else
										PrintHex(std::cout, sbase + sfi.offset, sfi.size);
									std::cout << std::endl;
								}
							}
						}
						else
						{
							const TypeInfo* ti = FindTypeByCode((int)fi.type);
							if (ti && ti->print)
							{
								// 使用 element_count 判断是否为数组
								int array_count = fi.element_count;
								int element_size = (array_count > 1) ? (fi.size / array_count) : fi.size;

								if (array_count > 1)
								{
									// 数组字段，打印所有元素
									std::cout << "[";
									for (int a = 0; a < array_count; a++)
									{
										if (a > 0) std::cout << ", ";
										ti->print(std::cout, base + fi.offset + a * element_size);
									}
									std::cout << "]";
								}
								else
								{
									// 标量字段（包括 String 类型）
									ti->print(std::cout, base + fi.offset);
								}
							}
							else
								PrintHex(std::cout, base + fi.offset, fi.size);
							std::cout << std::endl;
						}
					}
				}
				std::cout << "-------------------------------------" << std::endl;
				std::cout << "last write time: "
					<< std::asctime(std::localtime(&timestamp.tv_sec));
			}
		}
	}
}

void HandleSelect(std::string tagName)
{
	if (GlobalObj::qbdname.empty())
	{
		std::cout << "No db or board opened." << std::endl;
		return;
	}

	if (g_qbdtype == board)
	{
		SelectTag(tagName);
	}
}

void HandleDelete(std::string tagName)
{
	if (GlobalObj::qbdname.empty())
	{
		std::cout << "No db or board opened." << std::endl;
		return;
	}

	if (g_qbdtype == board)
	{
		DeleteTag(tagName);
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
			if (g_qbdtype == board)
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

	if (cmd == "openb" || cmd == "open")
	{
		HandleOpenBoard();
		return;
	}

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

	if (cmd == "types")
	{
		std::cout << "Built-in types:" << std::endl;
		for (auto& [name, info] : GetTypeByName())
			std::cout << "  " << name << "  (" << info.size << " bytes)" << std::endl;

		std::cout << std::endl << "Registered struct types:" << std::endl;
		for (auto& [name, info] : GetStructRegistry())
		{
			std::cout << "  " << name << "  (" << info->total_size << " bytes, "
				<< info->field_count << " fields)" << std::endl;
			for (int i = 0; i < info->field_count; i++)
			{
				const FieldInfo& f = info->fields[i];
				std::cout << "    +" << f.offset << "  " << f.name << "  ("
					<< f.size << " bytes)";
				if (f.type == Struct && f.struct_info)
					std::cout << "  -> " << f.struct_info->name;
				if (f.element_count > 1)
					std::cout << "  [" << f.element_count << "]";
				std::cout << std::endl;
			}
		}
		return;
	}

	std::cout << "Unknown command." << std::endl;
}
