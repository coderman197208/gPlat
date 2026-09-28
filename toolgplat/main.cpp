#include <cstdio>
// <cstdio> 必须在 readline 头文件之前包含，否则部分系统（如 CentOS/RedHat）上 readline 头文件会编译失败
#include <readline/readline.h>
#include <readline/history.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "../include/higplat.h"
#include "creation.h"
#include "display.h"
#include "record_input.h"
#include "text_util.h"
#include "type_handle.h"

namespace
{

constexpr int kGplatPort = 8777;
const char* const kDefaultHost = "127.0.0.1";
const char* const kBoardName = "BOARD";
constexpr int kQueueListBufferSize = 16384;   // 与服务端 MAXMSGLEN 一致

// 命令的作用域；Session 处于 Global 表示尚未打开 BOARD 或队列
enum class Scope { Global, Board, Queue };

struct QueueContext
{
	std::string name;
	QUEUE_HEAD head{};
	std::string typeName;              // 读不到类型描述符时为空
	const StructInfo* type = nullptr;  // 记录类型未在本地注册时为 nullptr
};

struct Session
{
	int conn = -1;
	std::string host;
	Scope scope = Scope::Global;
	QueueContext queue;
	bool running = true;

	bool IsConnected() const { return conn > 0; }

	std::string Prompt() const
	{
		if (!IsConnected())
			return "gplat>";
		switch (scope)
		{
		case Scope::Board: return host + ":" + kBoardName + ">";
		case Scope::Queue: return host + ":queue/" + queue.name + ">";
		default:           return host + ">";
		}
	}
};

using Words = std::vector<std::string>;

struct Command
{
	Scope scope;
	std::vector<std::string> names;   // 第一个为主名，其余为别名
	void (*handler)(Session& session, const Words& words);
	const char* help;
};

const std::vector<Command>& GetCommands();

bool IsAvailable(const Command& command, Scope scope)
{
	return command.scope == Scope::Global || command.scope == scope;
}

bool HasName(const Command& command, const std::string& name)
{
	return std::find(command.names.begin(), command.names.end(), name) != command.names.end();
}

// 优先返回当前作用域可用的命令，否则返回其他作用域的同名命令
const Command* FindCommand(const std::string& name, Scope scope)
{
	const Command* other = nullptr;
	for (const Command& command : GetCommands())
	{
		if (!HasName(command, name))
			continue;
		if (IsAvailable(command, scope))
			return &command;
		if (!other)
			other = &command;
	}
	return other;
}

const char* ScopeName(Scope scope)
{
	switch (scope)
	{
	case Scope::Board: return "board";
	case Scope::Queue: return "queue";
	default:           return "global";
	}
}

const char* OpenHint(Scope scope)
{
	return (scope == Scope::Queue) ? "open queue <queueName>" : "open board";
}

bool RequireConnection(const Session& session)
{
	if (session.IsConnected())
		return true;
	std::cout << "Not connected. Use 'conn [host]' first." << std::endl;
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

bool ListQueues(int conn, std::vector<std::string>& names, unsigned int& err)
{
	std::vector<char> buffer(kQueueListBufferSize);
	int count = 0;
	if (!listq(conn, buffer.data(), (int)buffer.size(), &count, &err))
		return false;

	const char* p = buffer.data();
	const char* end = p + buffer.size();
	for (int i = 0; i < count && p < end; i++)
	{
		const size_t len = strnlen(p, end - p);
		names.emplace_back(p, len);
		p += len + 1;
	}
	std::sort(names.begin(), names.end());
	return true;
}

void PrintQueueError(const std::string& action, const std::string& queueName, unsigned int err)
{
	switch (err)
	{
	case ERROR_RECORD_NOT_EXIST:
		std::cout << "Queue '" << queueName << "' not found. Type 'queues' to list loaded queues." << std::endl;
		break;
	case ERROR_OPERATE_PROHIBIT:
		std::cout << "'" << queueName << "' is not a queue." << std::endl;
		break;
	case ERROR_DQ_EMPTY:
		std::cout << "Queue '" << queueName << "' is empty." << std::endl;
		break;
	case ERROR_DQ_FULL:
		std::cout << "Queue '" << queueName << "' is full." << std::endl;
		break;
	default:
		std::cout << action << " queue '" << queueName << "' failed, error code " << err << "." << std::endl;
	}
}

// ---- 全局命令 ----
// ---- 全局命令 ----

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
	session.scope = Scope::Global;
}

std::string ReadQueueTypeName(int conn, const std::string& queueName)
{
	char descriptor[kTypeDescriptorBufferSize];
	int size = 0;
	unsigned int err = 0;
	TypeDescriptorHeader header;
	std::string className;
	if (readtype(conn, queueName.c_str(), "", descriptor, sizeof(descriptor), &size, &err)
		&& ParseTypeDescriptor(descriptor, std::min(size, (int)sizeof(descriptor)), header, className)
		&& header.typecode == kStructTypeCode)
		return className;
	return "";
}

void OpenQueue(Session& session, const std::string& queueName)
{
	QueueContext queue;
	queue.name = queueName;
	unsigned int err = 0;
	if (!readhead(session.conn, queueName.c_str(), &queue.head, &err))
	{
		PrintQueueError("Open", queueName, err);
		return;
	}
	queue.typeName = ReadQueueTypeName(session.conn, queueName);
	queue.type = FindStructByName(queue.typeName);
	if (queue.type && queue.type->total_size != queue.head.size)
	{
		std::cout << "Warning: local type '" << queue.typeName << "' is " << queue.type->total_size
			<< " bytes but queue record is " << queue.head.size << " bytes, records are shown as hex." << std::endl;
		queue.type = nullptr;
	}

	session.queue = queue;
	session.scope = Scope::Queue;
}

void CmdOpen(Session& session, const Words& words)
{
	if (!RequireConnection(session))
		return;

	const std::string target = (words.size() > 1) ? ToLower(words[1]) : "board";
	if (target == "board" && words.size() <= 2)
		session.scope = Scope::Board;
	else if (target == "queue" && words.size() == 3)
		OpenQueue(session, words[2]);
	else
		std::cout << "Usage: open [board] | open queue <queueName>" << std::endl;
}

void CmdClose(Session& session, const Words&)
{
	session.scope = Scope::Global;
}

void CmdQueues(Session& session, const Words&)
{
	if (!RequireConnection(session))
		return;

	std::vector<std::string> names;
	unsigned int err = 0;
	if (!ListQueues(session.conn, names, err))
	{
		std::cout << "List queues failed, error code " << err << "." << std::endl;
		return;
	}
	for (const std::string& name : names)
		std::cout << "  " << name << std::endl;
	std::cout << names.size() << " queue(s) loaded." << std::endl;
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
	if (!RequireConnection(session))
		return;

	if (words.size() > 1 && words[1] == "queue")
	{
		CreateQueueFromWords(session.conn, words);
		return;
	}

	if (session.scope != Scope::Board)
	{
		std::cout << "Tags can only be created in board context. Use 'open board' first." << std::endl;
		return;
	}

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

void PrintCommandList(const char* title, Scope scope)
{
	std::cout << title << std::endl;
	for (const Command& command : GetCommands())
	{
		if (command.scope != scope)
			continue;
		std::cout << "  " << command.names[0];
		for (size_t i = 1; i < command.names.size(); i++)
			std::cout << " | " << command.names[i];
		std::cout << std::endl;
	}
}

void CmdHelp(Session& session, const Words& words)
{
	if (words.size() == 1)
	{
		PrintCommandList("Global commands:", Scope::Global);
		if (session.scope == Scope::Board)
			PrintCommandList("Board commands:", Scope::Board);
		else if (session.scope == Scope::Queue)
			PrintCommandList("Queue commands:", Scope::Queue);
		std::cout << "Type 'help <command>' for details." << std::endl;
		return;
	}

	const std::string name = ToLower(words[1]);
	const Command* command = FindCommand(name, session.scope);
	if (!command)
	{
		std::cout << "No help available for command: " << words[1] << std::endl;
		return;
	}
	if (IsAvailable(*command, session.scope))
	{
		std::cout << command->help;
		return;
	}
	for (const Command& other : GetCommands())
		if (HasName(other, name))
			std::cout << "[" << ScopeName(other.scope) << " context]" << std::endl << other.help;
}

// ---- BOARD 命令 ----

void CmdSelect(Session& session, const Words& words)
{
	if (RequireWords(words, 2))
		PrintTag(session.conn, words[1]);
}

void CmdDelete(Session& session, const Words& words)
{
	if (!RequireWords(words, 2))
		return;

	unsigned int err = 0;
	if (deletetag(session.conn, words[1].c_str(), &err))
		std::cout << "Tag '" << words[1] << "' deleted." << std::endl;
	else
		std::cout << "Delete tag '" << words[1] << "' failed, error code " << err << "." << std::endl;
}

void CmdBoardDesc(Session& session, const Words&)
{
	PrintBoardInfo(session.conn);
}

bool RequireClearAll(const Words& words)
{
	if (words.size() == 2 && words[1] == "all")
		return true;
	std::cout << "Usage: clear all" << std::endl;
	return false;
}

void CmdBoardClear(Session& session, const Words& words)
{
	if (!RequireClearAll(words))
		return;

	unsigned int err = 0;
	if (clearb(session.conn, &err))
		std::cout << "Board cleared." << std::endl;
	else
		std::cout << "Clear board failed, error code " << err << "." << std::endl;
}

// ---- 队列命令 ----

void CmdQueueDesc(Session& session, const Words&)
{
	QueueContext& queue = session.queue;
	unsigned int err = 0;
	if (!readhead(session.conn, queue.name.c_str(), &queue.head, &err))
	{
		PrintQueueError("Describe", queue.name, err);
		return;
	}
	PrintQueueInfo(queue.name, queue.head, queue.typeName, queue.type);
}

void ShowQueueRecord(const Session& session, int position)
{
	const QueueContext& queue = session.queue;
	std::vector<char> record(queue.head.size);
	RECORD_HEAD recordHead{};
	unsigned int err = 0;
	if (!peekq(session.conn, queue.name.c_str(), position, record.data(), (int)record.size(), &recordHead, &err))
	{
		PrintQueueError("Peek", queue.name, err);
		return;
	}
	PrintQueueRecord(recordHead, queue.type, record.data(), (int)record.size());
}

void CmdPeek(Session& session, const Words&)
{
	ShowQueueRecord(session, PEEK_NEXT);
}

void CmdLast(Session& session, const Words&)
{
	ShowQueueRecord(session, PEEK_LATEST);
}

void CmdQueueClear(Session& session, const Words& words)
{
	if (!RequireClearAll(words))
		return;

	unsigned int err = 0;
	if (clearq(session.conn, session.queue.name.c_str(), &err))
		std::cout << "Queue '" << session.queue.name << "' cleared." << std::endl;
	else
		PrintQueueError("Clear", session.queue.name, err);
}

void CmdWrite(Session& session, const Words& words)
{
	const QueueContext& queue = session.queue;
	if (!queue.type)
	{
		std::cout << "Record type '" << (queue.typeName.empty() ? "<unknown>" : queue.typeName)
			<< "' is not usable locally, cannot write." << std::endl;
		return;
	}
	if (!RequireWords(words, 2))
		return;

	std::vector<char> record;
	std::string error;
	if (!BuildRecord(*queue.type, Words(words.begin() + 1, words.end()), record, error))
	{
		std::cout << error << std::endl;
		return;
	}

	unsigned int err = 0;
	if (writeq(session.conn, queue.name.c_str(), record.data(), (int)record.size(), &err))
		std::cout << "Record written." << std::endl;
	else
		PrintQueueError("Write", queue.name, err);
}

const std::vector<Command>& GetCommands()
{
	static const std::vector<Command> commands = {
		{Scope::Global, {"conn", "connect"}, CmdConnect,
R"(Usage: conn [host]
Description: Connects to the gPlat server (default 127.0.0.1).
)"},
		{Scope::Global, {"open", "openb"}, CmdOpen,
R"(Usage: open [board]
Description: Enters the board context. Subsequent select/create/delete/desc/clear act on BOARD.
--------------------------------------------------------------------------
Usage: open queue <queueName>
Description: Enters the context of the specified queue. Subsequent desc/peek/last/clear/write act on it.
Example: open queue myqueue
)"},
		{Scope::Global, {"close"}, CmdClose,
R"(Usage: close
Description: Leaves the current board or queue context.
)"},
		{Scope::Global, {"queues"}, CmdQueues,
R"(Usage: queues
Description: Lists the queues loaded by the server.
)"},
		{Scope::Global, {"create"}, CmdCreate,
R"(Usage: create <tagName> <typeName> [arraySize]
Description: Creates a new tag in BOARD (board context only) with the specified name, type, and optional array size.
typeName: Boolean | Int16 | UInt16 | Int32 | UInt32 | Int64 | UInt64 | Single | Double | String | <struct>
Example: create temperature Single
Example: create sensorValues Int32 10
Example: create alarmMessage String 80
Example: create sensor1 SensorData
Example: create sensorarray SensorData 3
--------------------------------------------------------------------------
Usage: create tag from config file <fileName>
Description: Creates tags based on a s7ioserver configuration file (board context only).
Example: create tag from config file plc_tags.ini
--------------------------------------------------------------------------
Usage: create tag from script file <fileName>
Description: Creates tags based on a script file (board context only).
Example: create tag from script file tags.txt
--------------------------------------------------------------------------
Usage: create queue <queueName> <typeName> <recordCount> [shift]
Description: Creates a new queue of a registered struct type; it can be opened immediately.
Example: create queue myqueue SensorData 10
Example: create queue myqueue SensorData 10 shift
)"},
		{Scope::Global, {"types"}, CmdTypes,
R"(Usage: types
Description: Lists built-in types and registered struct types.
)"},
		{Scope::Global, {"help"}, CmdHelp,
R"(Usage: help [command]
Description: Lists commands available in the current context or shows help for a command.
)"},
		{Scope::Global, {"exit", "q"}, CmdExit,
R"(Usage: exit
Description: Exits the tool.
)"},

		{Scope::Board, {"select"}, CmdSelect,
R"(Usage: select <tagName>
Description: Selects the specified tag and displays its value and metadata.
)"},
		{Scope::Board, {"delete"}, CmdDelete,
R"(Usage: delete <tagName>
Description: Deletes the specified tag from BOARD.
)"},
		{Scope::Board, {"desc"}, CmdBoardDesc,
R"(Usage: desc
Description: Describes BOARD, showing total size, remaining size, and tag count.
)"},
		{Scope::Board, {"clear"}, CmdBoardClear,
R"(Usage: clear all
Description: Clears all tags from BOARD.
)"},

		{Scope::Queue, {"desc"}, CmdQueueDesc,
R"(Usage: desc
Description: Describes the current queue: record type, mode, read/write points, pending count and fields.
)"},
		{Scope::Queue, {"peek"}, CmdPeek,
R"(Usage: peek
Description: Shows the record the next read will return, without consuming it.
             For a shift queue this is the latest record, same as 'last'.
)"},
		{Scope::Queue, {"last"}, CmdLast,
R"(Usage: last
Description: Shows the most recently written record, without consuming it.
)"},
		{Scope::Queue, {"clear"}, CmdQueueClear,
R"(Usage: clear all
Description: Clears all records from the current queue.
)"},
		{Scope::Queue, {"write"}, CmdWrite,
R"(Usage: write <field>=<value> [<field>=<value> ...]
Description: Writes one record to the current queue; unassigned fields are zero.
             Subscribers of the queue are notified.
field: name | name[i] | name.member | name[i].member
Array fields without index take comma-separated values; quote values containing spaces.
Example: write temperature=25 humidity=60 pressure=1.013 alarm=true location="Room 1"
Example: write speed=1.5,2,3 motor_name[0]=M1
Example: write id=7 pos.latitude=31.2 history[1].longitude=121.5
)"},
	};
	return commands;
}

void PrintUnavailable(const std::string& name)
{
	std::string scopes;
	std::string hints;
	for (const Command& command : GetCommands())
	{
		if (!HasName(command, name))
			continue;
		scopes += (scopes.empty() ? "" : " or ") + std::string(ScopeName(command.scope));
		hints += (hints.empty() ? "'" : " or '") + std::string(OpenHint(command.scope)) + "'";
	}
	std::cout << "'" << name << "' is only available in " << scopes << " context. Use " << hints << " first." << std::endl;
}

void Execute(Session& session, const Words& words)
{
	const std::string name = ToLower(words[0]);
	const Command* command = FindCommand(name, session.scope);
	if (!command)
	{
		std::cout << "Unknown command. Type 'help' for a list of commands." << std::endl;
		return;
	}
	if (!IsAvailable(*command, session.scope))
	{
		PrintUnavailable(name);
		return;
	}

	// higplat 遇到致命错误码会抛出异常，不应让交互工具退出
	try
	{
		command->handler(session, words);
	}
	catch (const std::exception& e)
	{
		std::cout << "Command failed: " << e.what() << std::endl;
	}
}

// ---- Tab 补全 ----

const Session* g_session = nullptr;   // readline 补全回调无法携带上下文
std::vector<std::string> g_candidates;

char* GenerateCandidate(const char* text, int state)
{
	static size_t index = 0;
	if (state == 0)
		index = 0;
	const size_t length = strlen(text);
	while (index < g_candidates.size())
	{
		const std::string& candidate = g_candidates[index++];
		if (candidate.compare(0, length, text) == 0)
			return strdup(candidate.c_str());
	}
	return nullptr;
}

// nullopt: 交给 readline 默认的文件名补全
std::optional<std::vector<std::string>> CompletionCandidates(const Session& session, const Words& previous)
{
	const bool completingCommand = previous.empty() || (previous.size() == 1 && ToLower(previous[0]) == "help");
	if (completingCommand)
	{
		std::vector<std::string> names;
		for (const Command& command : GetCommands())
			if (IsAvailable(command, session.scope))
				names.insert(names.end(), command.names.begin(), command.names.end());
		return names;
	}

	const std::string command = ToLower(previous[0]);
	if (command != "open" && command != "openb")
		return std::nullopt;
	if (previous.size() == 1)
		return std::vector<std::string>{"board", "queue"};

	std::vector<std::string> names;
	unsigned int err = 0;
	if (previous.size() == 2 && ToLower(previous[1]) == "queue" && session.IsConnected())
		ListQueues(session.conn, names, err);
	return names;
}

char** Complete(const char* text, int start, int)
{
	const Words previous = SplitWords(std::string(rl_line_buffer, start));
	std::optional<std::vector<std::string>> candidates = CompletionCandidates(*g_session, previous);
	if (!candidates)
		return nullptr;

	rl_attempted_completion_over = 1;
	g_candidates = std::move(*candidates);
	return rl_completion_matches(text, GenerateCandidate);
}

} // namespace

int main()
{
	Session session;
	g_session = &session;
	rl_attempted_completion_function = Complete;

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
