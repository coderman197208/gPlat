#ifndef MSG_H_
#define MSG_H_

#define MAXMSGLEN 16384

//必须和ngx_c_slogic.cxx中statusHandler[]的下标对应上
enum MSGID
{
	SUCCEED = 5,
	FAIL,
	CONNECT,
	RECONNECT,
	DISCONNECT,
	OPEN,
	OPENQ,        //OPENB
	CLOSEQ,       //CLOSEB
	CLEARQ,
	ISEMPTYQ,
	ISFULLQ,
	READQ,
	PEEKQ,
	WRITEQ,
	READB,
	READBSTRING,
	WRITEB,
	QDATA,
	READHEAD,
	MULREADQ,
	SETPTRQ,
	WATCHDOG,
	SELECTTB,
	CLEARTB,
	INSERTTB,
	REFRESHTB,
	READTYPE,
	CREATEITEM,
	CREATETABLE,
	DELETEITEM,
	DELETETABLE,
	READHEADB,
	READHEADDB,
	ACK,
	POPARECORDQ,
	WRITEBSTRING,
	WRITETOL1,
	SUBSCRIBE,
	CANCELSUBSCRIBE,
	POST,
	POSTWAIT,
	PASSTOSERVER,		//mark 内部使用
	CLEARB,
	CLEARDB,
	REGISTERPLCSERVER,
	WRITEBPLC,
	WRITEBSTRINGPLC,
	READBOARDINFO,
	CREATEQUEUE,
};

#pragma pack( push, enter_MSG_H_, 1)

typedef struct {
	int    id;
	char   qname[40];
	char   itemname[40];
	int    qbdtype;
	int    datasize;
	int    datatype;
	timespec timestamp;
	int    start;
	int    count;
	int    recsize;
	int    bodysize;
	int    readptr;
	int    writeptr;
	unsigned int  error;
	int    subsize;
	int    offset;
	char   ip[16];
	int    arraysize;	// used for plc array write
	int	   eventid;
	int    eventarg;
	int    timeout;
} MSGHEAD, * PMSGHEAD;

typedef MSGHEAD PKGHEAD;
typedef MSGHEAD* PPKGHEAD;

typedef struct {
	MSGHEAD head;
	char    body[MAXMSGLEN];
} MSGSTRUCT, *PMSGSTRUCT;

#pragma pack( pop, enter_MSG_H_ )

#endif	/*MSG_H_*/