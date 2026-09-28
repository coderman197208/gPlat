#include <cstdio>
// <cstdio> 必须在 readline 头文件之前包含，否则部分系统（如 CentOS/RedHat）上 readline 头文件会编译失败
#include <readline/readline.h>
#include <readline/history.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "../include/higplat.h"
#include "creation.h"
#include "display.h"
#include "text_util.h"

namespace
{

constexpr int kGplatPort = 8777;
const char* const kDefaultHost = "127.0.0.1";

struct Session
{
	int conn = -1;
	std::string host;
	bool boardOpened = false;
	bool running = true;

	bool IsConnected() const { return conn > 0; }

	std::string Prompt() const
	{
		if (!IsConnected())
			return "gplat>";
		return boardOpened ? host + ".BOARD>" : host + ">";
	}
};

using Words = std::vector<std::string>;

struct Command
{
	std::vector<std::string> names;   // 第一个为主名，其余为别名
	void (*handler)(Session& session, const Words& words);
	const char* help;
};

const std::vector<Command>& GetCommands();

const Command* FindCommand(const std::string& name)
{
	for (const Command& command : GetCommands())
		for (const std::string& alias : command.names)
			if (alias == name)
				return &command;
	return nullptr;
}

bool RequireConnection(const Session& session)
{
	if (session.IsConnected())
		return true;
	std::cout << "Not connected. Use 'conn [host]' first." << std::endl;
	return false;
}

bool RequireBoard(const Session& session)
{
	if (!RequireConnection(session))
		return false;
	if (session.boardOpened)
		return true;
	std::cout << "No board opened. Use 'open' first." << std::endl;
	return false;
}

// words 至少需要 count 个，否则提示用户查看帮助
bool RequireWords(const Words& words, size_t count)
{
	if (words.size() >= count)
		return true;
	std::cout << "Missing arguments. Type 'help " << words[0] << "' for usage." << std::endl;
	return false;
}

void CmdConnect(Session& session, const Words& words)
{
	const std::string host = (words.size() > 1) ? words[1] : kDefaultHost;
	int conn = connectgplat(host.c_str(), kGplatPort);
	if (conn <= 0)
	{
		std::cout << "无法连接到" << host << "." << std::endl;
		return;
	}

	if (session.IsConnected())
		disconnectgplat(session.conn);
	session.conn = conn;
	session.host = host;
	session.boardOpened = false;
}

void CmdOpen(Session& session, const Words&)
{
	if (RequireConnection(session))
		session.boardOpened = true;
}

void CmdSelect(Session& session, const Words& words)
{
	if (RequireBoard(session) && RequireWords(words, 2))
		PrintTag(session.conn, words[1]);
}

void CmdDelete(Session& session, const Words& words)
{
	if (!RequireBoard(session) || !RequireWords(words, 2))
		return;

	unsigned int err = 0;
	if (deletetag(session.conn, words[1].c_str(), &err))
		std::cout << "Tag '" << words[1] << "' deleted." << std::endl;
	else
		std::cout << "Delete tag '" << words[1] << "' failed, error code " << err << "." << std::endl;
}

void CmdDesc(Session& session, const Words&)
{
	if (RequireBoard(session))
		PrintBoardInfo(session.conn);
}

void CmdClear(Session& session, const Words& words)
{
	if (!RequireBoard(session))
		return;
	if (words.size() != 2 || words[1] != "all")
	{
		std::cout << "Usage: clear all" << std::endl;
		return;
	}

	unsigned int err = 0;
	if (clearb(session.conn, &err))
		std::cout << "Board cleared." << std::endl;
	else
		std::cout << "Clear board failed, error code " << err << "." << std::endl;
}

// create queue <queueName> <typeName> <recordCount> [shift]
void CreateQueueFromWords(int conn, const Words& words)
{
	const bool validMode = (words.size() == 5) || (words.size() == 6 && words[5] == "shift");
	if (!validMode)
	{
		std::cout << "Usage: create queue <queueName> <typeName> <recordCount> [shift]" << std::endl;
		return;
	}

	int recordCount = 0;
	if (!ParseInt(words[4], recordCount) || recordCount <= 0)
	{
		std::cout << "Record count must be a positive integer: " << words[4] << std::endl;
		return;
	}
	CreateQueue(conn, words[2], words[3], recordCount, words.size() == 6);
}

void CmdCreate(Session& session, const Words& words)
{
	if (words.size() > 1 && words[1] == "queue")
	{
		if (RequireConnection(session))
			CreateQueueFromWords(session.conn, words);
		return;
	}

	if (!RequireBoard(session))
		return;

	// create tag from <config|script> file <fileName>
	if (words.size() == 6)
	{
		const std::string source = ToLower(words[3]);
		if (source == "config")
			CreateTagsFromConfigFile(session.conn, words[5]);
		else if (source == "script")
			CreateTagsFromScriptFile(session.conn, words[5]);
		else
			std::cout << "Usage: create tag from <config|script> file <fileName>" << std::endl;
		return;
	}

	CreateTagFromWords(session.conn, words);
}

void CmdTypes(Session&, const Words&)
{
	PrintTypes();
}

void CmdExit(Session& session, const Words&)
{
	session.running = false;
}

void CmdHelp(Session&, const Words& words)
{
	if (words.size() == 1)
	{
		std::cout << "Available commands:" << std::endl;
		for (const Command& command : GetCommands())
		{
			std::cout << "  " << command.names[0];
			for (size_t i = 1; i < command.names.size(); i++)
				std::cout << " | " << command.names[i];
			std::cout << std::endl;
		}
		std::cout << "Type 'help <command>' for details." << std::endl;
		return;
	}

	const Command* command = FindCommand(ToLower(words[1]));
	if (command)
		std::cout << command->help;
	else
		std::cout << "No help available for command: " << words[1] << std::endl;
}

const std::vector<Command>& GetCommands()
{
	static const std::vector<Command> commands = {
		{{"conn", "connect"}, CmdConnect,
R"(Usage: conn [host]
Description: Connects to the gPlat server (default 127.0.0.1).
)"},
		{{"open", "openb"}, CmdOpen,
R"(Usage: open
Description: Opens the board of the connected server.
)"},
		{{"select"}, CmdSelect,
R"(Usage: select <tagName>
Description: Selects the specified tag and displays its value and metadata.
)"},
		{{"create"}, CmdCreate,
R"(Usage: create <tagName> <typeName> [arraySize]
Description: Creates a new tag with the specified name, type, and optional array size.
typeName: Boolean | Int16 | UInt16 | Int32 | UInt32 | Int64 | UInt64 | Single | Double | String | <struct>
Example: create temperature Single
Example: create sensorValues Int32 10
Example: create alarmMessage String 80
Example: create sensor1 SensorData
Example: create sensorarray SensorData 3
--------------------------------------------------------------------------
Usage: create tag from config file <fileName>
Description: Creates tags based on a s7ioserver configuration file.
Example: create tag from config file plc_tags.ini
--------------------------------------------------------------------------
Usage: create tag from script file <fileName>
Description: Creates tags based on a script file.
Example: create tag from script file tags.txt
--------------------------------------------------------------------------
Usage: create queue <queueName> <typeName> <recordCount> [shift]
Description: Creates a new queue of a registered struct type.
Example: create queue myqueue SensorData 10
Example: create queue myqueue SensorData 10 shift
)"},
		{{"delete"}, CmdDelete,
R"(Usage: delete <tagName>
Description: Deletes the specified tag from the current board.
)"},
		{{"desc"}, CmdDesc,
R"(Usage: desc
Description: Describes the current board, showing total size, remaining size, and tag count.
)"},
		{{"clear"}, CmdClear,
R"(Usage: clear all
Description: Clears all tags from the current board.
)"},
		{{"types"}, CmdTypes,
R"(Usage: types
Description: Lists built-in types and registered struct types.
)"},
		{{"help"}, CmdHelp,
R"(Usage: help [command]
Description: Lists commands or shows help for a command.
)"},
		{{"exit", "q"}, CmdExit,
R"(Usage: exit
Description: Exits the tool.
)"},
	};
	return commands;
}

void Execute(Session& session, const Words& words)
{
	const Command* command = FindCommand(ToLower(words[0]));
	if (command)
		command->handler(session, words);
	else
		std::cout << "Unknown command. Type 'help' for a list of commands." << std::endl;
}

} // namespace

int main()
{
	rl_bind_key('\t', rl_complete);

	Session session;
	char* line;
	while (session.running && (line = readline(session.Prompt().c_str())) != nullptr)
	{
		if (*line)
			add_history(line);
		Words words = SplitWords(line);
		free(line);

		if (!words.empty())
			Execute(session, words);
	}

	if (session.IsConnected())
		disconnectgplat(session.conn);
	return 0;
}
