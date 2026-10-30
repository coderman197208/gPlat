// Local board API: tags in the mmap'd BOARD file loaded by this process.
//
// Locking (lock order: index lock -> stripe locks 0..MUTEXSIZE-1): the index lock BOARD_HEAD::mutex_rw guards the
// hash index and the layout; a stripe lock (mutex_rw_tag, by slot) guards an item's data. Reads and writes look the
// item up under the index lock, take its stripe lock and only then release the index lock. Operations that move or
// reset the data area (DeleteItem, ClearB) hold the index lock and every stripe lock.
#include <cstddef>
#include <cstring>
#include <ctime>
#include <mutex>

#include "qbd.h"

namespace {

// Head of a loaded board, or nullptr with g_lastError set
BOARD_HEAD* FetchBoard(const char* name, TABLE_MSG& tab)
{
	if (!fetchtab(name, tab))
		return nullptr;
	auto* head = static_cast<BOARD_HEAD*>(tab.lpMapAddress);
	if (head->qbdtype != BOARD_T) {
		g_lastError = ERROR_OPERATE_PROHIBIT;
		return nullptr;
	}
	return head;
}

// Looks a live item up under the index lock; lockItem() then trades the index lock for the item's stripe lock.
class ItemAccess
{
public:
	ItemAccess(const TABLE_MSG& tab, BOARD_HEAD* head, const char* name)
		: m_head(head), m_indexLock(*tab.pmutex_rw), m_loc(FindBoardItem(head->index, name)) {}

	bool found() const { return m_loc >= 0; }
	BOARD_INDEX_STRUCT& item() const { return m_head->index[m_loc]; }
	char* data() const { return BoardData(m_head) + item().startpos; }

	// 先持条带锁再释放全局锁，避免 DeleteItem 在两锁之间搬移/擦除该 tag
	void lockItem()
	{
		m_itemLock = std::unique_lock<std::mutex>(StripeMutex(m_head, m_loc));
		m_indexLock.unlock();
	}

private:
	BOARD_HEAD* m_head;
	std::unique_lock<std::mutex> m_indexLock;
	int m_loc;
	std::unique_lock<std::mutex> m_itemLock;
};

// Holds every stripe lock of a board (caller holds the index lock), for operations that move or reset the data area
class StripeLockAll
{
public:
	explicit StripeLockAll(BOARD_HEAD* head) : m_head(head)
	{
		for (std::mutex& stripe : m_head->mutex_rw_tag)
			stripe.lock();
	}
	~StripeLockAll()
	{
		for (int i = MUTEXSIZE - 1; i >= 0; i--)
			m_head->mutex_rw_tag[i].unlock();
	}
	StripeLockAll(const StripeLockAll&) = delete;
	StripeLockAll& operator=(const StripeLockAll&) = delete;

private:
	BOARD_HEAD* m_head;
};

// Copies a string item (its actual length, no '\0' added); zeroFill clears the whole buffer first
bool ReadStringItem(const char* boardName, const char* itemName, void* lpItem, int actSize, bool zeroFill,
	int* strLength, timespec* timestamp)
{
	TABLE_MSG tab;
	BOARD_HEAD* head = FetchBoard(boardName, tab);
	if (head == nullptr)
		return false;

	ItemAccess access(tab, head, itemName);
	if (!access.found()) {
		g_lastError = ERROR_TAG_NOT_EXIST;
		return false;
	}

	// strlenth changes under the stripe lock: check it there so that the copy fits the buffer
	access.lockItem();
	const BOARD_INDEX_STRUCT& item = access.item();
	if (item.strlenth > actSize) {
		g_lastError = BUFFER_TOO_SMALL;
		return false;
	}

	if (zeroFill)
		memset(lpItem, 0, actSize);
	memcpy(lpItem, access.data(), item.strlenth);
	if (timestamp != nullptr)
		*timestamp = item.timestamp;
	if (strLength != nullptr)
		*strLength = item.strlenth;
	return true;
}

} // namespace

// size: data area size; the file adds BOARD_HEAD and the type area
extern "C" bool CreateB(const char* lpFileName, int size)
{
	const int fileSize = sizeof(BOARD_HEAD) + size + INDEXSIZE * TYPEAVGSIZE;
	return CreateQbdFile(lpFileName, fileSize, [size](char* base) {
		auto* head = reinterpret_cast<BOARD_HEAD*>(base);
		head->qbdtype = BOARD_T;
		head->totalsize = sizeof(BOARD_HEAD) + size;	// BOARD_HEAD和后面数据区大小的和，不包括最后面的类型区
		head->typesize = INDEXSIZE * TYPEAVGSIZE;		// 最后面的类型区的大小
		head->remain = size;
		head->typeremain = INDEXSIZE * TYPEAVGSIZE;
	});
}

// Board sizes and, when ppBuff != nullptr and *buffSize is large enough, the item names (truncated to 23 chars)
extern "C" bool ReadInfoB(const char* lpBulletinName, int* pTotalSize, int* pDataSize, int* pLeftSize, int* pItemNum,
	int* buffSize, char ppBuff[][24])
{
	TABLE_MSG tab;
	BOARD_HEAD* head = FetchBoard(lpBulletinName, tab);
	if (head == nullptr)
		return false;

	std::lock_guard<std::mutex> lock(*tab.pmutex_rw);
	if (pTotalSize != nullptr)
		*pTotalSize = head->totalsize;
	if (pDataSize != nullptr)
		*pDataSize = head->totalsize - sizeof(BOARD_HEAD);
	if (pLeftSize != nullptr)
		*pLeftSize = head->remain;
	if (pItemNum != nullptr)
		*pItemNum = head->indexcount;

	if (buffSize != nullptr) {
		if (head->indexcount > *buffSize) {
			*buffSize = 0;
		}
		else if (ppBuff != nullptr) {
			*buffSize = head->indexcount;
			int count = 0;
			for (const BOARD_INDEX_STRUCT& item : head->index) {
				if (item.itemname[0] == '\0' || item.erased)
					continue;
				if (count < head->indexcount) {
					strncpy(ppBuff[count], item.itemname, sizeof(ppBuff[count]) - 1);
					ppBuff[count][sizeof(ppBuff[count]) - 1] = '\0';
				}
				count++;
			}
			if (count != head->indexcount)
				return false;
		}
	}
	return true;
}

extern "C" bool ReadB(const char* lpBulletinName, const char* lpItemName, void* lpItem, int actSize, timespec* timestamp)
{
	TABLE_MSG tab;
	BOARD_HEAD* head = FetchBoard(lpBulletinName, tab);
	if (head == nullptr)
		return false;

	ItemAccess access(tab, head, lpItemName);
	if (!access.found()) {
		g_lastError = ERROR_TAG_NOT_EXIST;
		return false;
	}
	if (access.item().itemsize != actSize) {
		g_lastError = ERROR_RECORDSIZE;
		return false;
	}

	access.lockItem();
	memcpy(lpItem, access.data(), actSize);
	if (timestamp != nullptr)
		*timestamp = access.item().timestamp;
	return true;
}

extern "C" bool ExistsB(const char* lpBulletinName, const char* lpItemName)
{
	TABLE_MSG tab;
	BOARD_HEAD* head = FetchBoard(lpBulletinName, tab);
	if (head == nullptr)
		return false;

	std::lock_guard<std::mutex> lock(*tab.pmutex_rw);
	if (FindBoardItem(head->index, lpItemName) < 0) {
		g_lastError = ERROR_TAG_NOT_EXIST;
		return false;
	}
	return true;
}

// 返回字符串的实际长度（不包括'\0'结尾），不存在返回 -1
extern "C" int GetStringLength(const char* lpBulletinName, const char* lpItemName)
{
	TABLE_MSG tab;
	BOARD_HEAD* head = FetchBoard(lpBulletinName, tab);
	if (head == nullptr)
		return -1;

	std::lock_guard<std::mutex> lock(*tab.pmutex_rw);
	const int loc = FindBoardItem(head->index, lpItemName);
	return loc < 0 ? -1 : head->index[loc].strlenth;
}

// 缓冲区清零后拷贝实际长度的字符串；actSize 恰好等于字符串长度时结果不以'\0'结尾
extern "C" bool ReadB_String(const char* lpBulletinName, const char* lpItemName, void* lpItem, int actSize, timespec* timestamp)
{
	return ReadStringItem(lpBulletinName, lpItemName, lpItem, actSize, true, nullptr, timestamp);
}

// 只拷贝实际长度的字符串、不补'\0'，*strLength 返回实际长度（不包括'\0'结尾），由调用方决定是否添加'\0'
extern "C" bool ReadB_String2(const char* lpBulletinName, const char* lpItemName, void* lpItem, int actSize, int* strLength, timespec* timestamp)
{
	return ReadStringItem(lpBulletinName, lpItemName, lpItem, actSize, false, strLength, timestamp);
}

// Writes the whole item, or when lpSubItem != nullptr only the actSubSize bytes at lpSubItem inside lpItem
extern "C" bool WriteB(const char* lpBulletinName, const char* lpItemName, void* lpItem, int actSize, void* lpSubItem, int actSubSize)
{
	TABLE_MSG tab;
	BOARD_HEAD* head = FetchBoard(lpBulletinName, tab);
	if (head == nullptr)
		return false;

	ItemAccess access(tab, head, lpItemName);
	if (!access.found()) {
		g_lastError = ERROR_TAG_NOT_EXIST;
		return false;
	}

	access.lockItem();
	BOARD_INDEX_STRUCT& item = access.item();
	if (lpSubItem == nullptr) {
		if (item.itemsize != actSize) {
			g_lastError = ERROR_RECORDSIZE;
			return false;
		}
		memcpy(access.data(), lpItem, actSize);
	}
	else {
		const std::ptrdiff_t offset = static_cast<char*>(lpSubItem) - static_cast<char*>(lpItem);
		if (item.itemsize != actSize || offset < 0 || actSubSize < 0 || offset + actSubSize > actSize) {
			g_lastError = ERROR_RECORDSIZE;
			return false;
		}
		memcpy(access.data() + offset, lpSubItem, actSubSize);
	}
	clock_gettime(CLOCK_REALTIME, &item.timestamp);
	return true;
}

// lpSubItem must be nullptr: partial string writes are not supported
extern "C" bool WriteB_String(const char* lpBulletinName, const char* lpItemName, void* lpItem, int actSize, void* lpSubItem, int actSubSize)
{
	(void)actSubSize;
	TABLE_MSG tab;
	BOARD_HEAD* head = FetchBoard(lpBulletinName, tab);
	if (head == nullptr)
		return false;

	ItemAccess access(tab, head, lpItemName);
	if (!access.found()) {
		g_lastError = ERROR_TAG_NOT_EXIST;
		return false;
	}

	access.lockItem();
	if (lpSubItem != nullptr)
		return false;

	BOARD_INDEX_STRUCT& item = access.item();
	if (actSize < 0) {
		g_lastError = ERROR_PARAMETER_SIZE;
		return false;
	}
	if (item.itemsize < actSize) {
		g_lastError = STRING_TOO_LONG;
		return false;
	}

	item.strlenth = actSize;	// 记录字符串长度，便于读取时判断缓冲区是否足够
	memset(access.data(), 0, item.itemsize);
	memcpy(access.data(), lpItem, actSize);
	clock_gettime(CLOCK_REALTIME, &item.timestamp);
	return true;
}

// Writes the actSubSize bytes at offSet inside lpItem
extern "C" bool WriteBOffSet(const char* lpBulletinName, const char* lpItemName, void* lpItem, int actSize, int offSet, int actSubSize)
{
	return WriteB(lpBulletinName, lpItemName, lpItem, actSize, static_cast<char*>(lpItem) + offSet, actSubSize);
}

// Removes every item
extern "C" bool ClearB(const char* lpBoardName)
{
	TABLE_MSG tab;
	BOARD_HEAD* head = FetchBoard(lpBoardName, tab);
	if (head == nullptr)
		return false;

	std::lock_guard<std::mutex> lock(*tab.pmutex_rw);
	StripeLockAll stripes(head);

	memset(head->index, 0, sizeof(head->index));
	head->remain = head->totalsize - sizeof(BOARD_HEAD);
	head->typeremain = head->typesize;
	head->counter = 0;
	head->nextpos = 0;
	head->nexttypepos = 0;
	head->indexcount = 0;
	return true;
}

// Removes an item and compacts the data and type areas
extern "C" bool DeleteItem(const char* lpBoardName, const char* lpItemName)
{
	TABLE_MSG tab;
	BOARD_HEAD* head = FetchBoard(lpBoardName, tab);
	if (head == nullptr)
		return false;

	std::lock_guard<std::mutex> lock(*tab.pmutex_rw);
	// memmove 会搬移其它 tag 的数据，必须排除所有持条带锁的读写者
	StripeLockAll stripes(head);

	const int loc = FindBoardItem(head->index, lpItemName);
	if (loc < 0) {
		g_lastError = ERROR_ITEM_NOT_EXIST;
		return false;
	}

	BOARD_INDEX_STRUCT& item = head->index[loc];
	strcpy(item.itemname, kErasedName);
	item.erased = true;

	const int pos = item.startpos;
	const int itemsize = item.itemsize;
	char* data = BoardData(head);
	const int dataLength = head->totalsize - sizeof(BOARD_HEAD) - pos - itemsize;
	memmove(data + pos, data + pos + itemsize, dataLength);

	const int typepos = item.typeaddr;
	const int typesize = item.typesize;
	item.typesize = 0;
	char* types = BoardTypeArea(head);
	memmove(types + typepos, types + typepos + typesize, INDEXSIZE * TYPEAVGSIZE - typepos - typesize);

	for (BOARD_INDEX_STRUCT& other : head->index) {
		if (other.itemname[0] == '\0' || other.erased)
			continue;
		if (other.startpos > pos)
			other.startpos -= itemsize;
		if (other.typeaddr > typepos)
			other.typeaddr -= typesize;
	}

	head->nextpos -= itemsize;
	head->remain += itemsize;
	head->indexcount--;
	head->nexttypepos -= typesize;
	head->typeremain += typesize;
	return true;
}

// Creates a zeroed item, with its type descriptor when pType != nullptr
extern "C" bool CreateItem(const char* lpBoardName, const char* lpItemName, int itemSize, void* pType, int typeSize)
{
	// 负数会绕过下面的剩余空间检查并使 memset/memcpy 长度变成超大 size_t（server 经网络可达）
	if (itemSize < 0 || typeSize < 0 || typeSize > TYPEMAXSIZE
		|| strnlen(lpItemName, MAXDQNAMELENTH) >= MAXDQNAMELENTH) {
		g_lastError = ERROR_PARAMETER_SIZE;
		return false;
	}

	TABLE_MSG tab;
	BOARD_HEAD* head = FetchBoard(lpBoardName, tab);
	if (head == nullptr)
		return false;

	std::lock_guard<std::mutex> lock(*tab.pmutex_rw);
	BOARD_INDEX_STRUCT* index = head->index;

	// an empty name stops at a never-used slot and counts as existing too
	const int existing = BoardProbe(index, lpItemName);
	if (strcmp(index[existing].itemname, lpItemName) == 0 && !index[existing].erased) {
		g_lastError = ERROR_ITEM_ALREADY_EXIST;
		return false;
	}

	if (head->remain < itemSize) {
		g_lastError = ERROR_NO_SPACE;
		return false;
	}

	// first free slot (never used or erased) on the probe chain
	int loc = hash1(lpItemName);
	const int c = hash2(lpItemName);
	while (index[loc].itemname[0] != '\0' && !index[loc].erased)
		loc = (loc + c) % INDEXSIZE;

	if (head->indexcount == INDEXSIZE - 1) {
		g_lastError = ERROR_ITEM_OVERFLOW;
		return false;
	}

	BOARD_INDEX_STRUCT& item = index[loc];
	head->indexcount++;
	strcpy(item.itemname, lpItemName);
	item.itemsize = itemSize;
	item.startpos = head->nextpos;
	item.strlenth = 0;	// the slot may be reused from a deleted item
	item.erased = false;
	head->nextpos += itemSize;
	head->remain -= itemSize;
	clock_gettime(CLOCK_REALTIME, &item.timestamp);

	if (pType != nullptr && typeSize != 0 && typeSize < TYPEMAXSIZE && typeSize < head->typeremain) {
		memcpy(BoardTypeArea(head) + head->nexttypepos, pType, typeSize);
		item.typesize = typeSize;
		item.typeaddr = head->nexttypepos;
		head->nexttypepos += typeSize;
		head->typeremain -= typeSize;
	}
	else {
		item.typesize = 0;
	}
	memset(BoardData(head) + item.startpos, 0, itemSize);
	return true;
}

extern "C" bool ReadBoardInfo(const char* lpBoardName, BOARD_INFO* boardinfo)
{
	TABLE_MSG tab;
	BOARD_HEAD* head = FetchBoard(lpBoardName, tab);
	if (head == nullptr)
		return false;

	boardinfo->tagcount_head = head->indexcount;
	boardinfo->totalsize = head->totalsize - sizeof(BOARD_HEAD);
	boardinfo->remainsize = head->remain;

	int live = 0;
	for (const BOARD_INDEX_STRUCT& item : head->index) {
		if (item.itemname[0] != '\0' && !item.erased)
			live++;
	}
	boardinfo->tagcount_act = live;
	return true;
}

// 从索引槽 start 起列出 tag 元数据（格式见 TAG_META），缓冲区放不下时 *next 为下一页起始槽，全部列完为 -1
extern "C" bool ListTags(const char* lpBoardName, int start, char* buff, int buffSize, int* bytes, int* count, int* next)
{
	*bytes = 0;
	*count = 0;
	*next = -1;
	if (start < 0 || start > INDEXSIZE) {
		g_lastError = ERROR_INVALID_PARAMETER;
		return false;
	}

	TABLE_MSG tab;
	BOARD_HEAD* head = FetchBoard(lpBoardName, tab);
	if (head == nullptr)
		return false;

	std::lock_guard<std::mutex> lock(*tab.pmutex_rw);
	for (int i = start; i < INDEXSIZE; i++) {
		const BOARD_INDEX_STRUCT& item = head->index[i];
		if (item.itemname[0] == '\0' || item.erased)
			continue;

		const TAG_META meta{ item.itemsize, item.typesize };
		const int nameLen = static_cast<int>(strnlen(item.itemname, MAXDQNAMELENTH - 1));
		const int len = static_cast<int>(sizeof(meta)) + nameLen + 1 + meta.typesize;
		if (*bytes + len > buffSize) {
			if (*count == 0) {
				g_lastError = ERROR_BUFFER_TOO_SMALL;
				return false;
			}
			*next = i;
			return true;
		}

		char* p = buff + *bytes;
		memcpy(p, &meta, sizeof(meta));
		p += sizeof(meta);
		memcpy(p, item.itemname, nameLen);
		p[nameLen] = '\0';
		memcpy(p + nameLen + 1, BoardTypeArea(head) + item.typeaddr, meta.typesize);
		*bytes += len;
		(*count)++;
	}
	return true;
}
