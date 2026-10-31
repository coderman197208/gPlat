#pragma once  
#include <list>  
#include <map>  
#include <string>  
#include <shared_mutex>  
#include <cstring>
#include <cstdint>
#include <utility>
#include <mutex> // Add this header to fix the 'unique_lock' issue  

enum EVENTID  
{  
    DEFAULT = 1,  
    POST_DELAY = 2,  
    NOT_EQUAL_ZERO = 4,  
    EQUAL_ZERO = 8,  
};  

struct EventNode  
{  
    void* subscriber;  //lpngx_connection_t
    uint64_t  seq;     //订阅时连接的实例序号（iCurrsequence）；投递时据此识别已断开或已被复用的连接
    char      eventname[40];  
    EVENTID   eventid;  
    int       eventarg;  
};  

// PLC IO 服务器注册：连接 + 注册时的实例序号
struct PlcIoServer
{
    void*    subscriber = nullptr;  //lpngx_connection_t
    uint64_t seq = 0;
};

class CSubscribe  
{  
private:  
    std::shared_mutex mutex_rw;  
    std::map<std::string, std::list<EventNode>> m_mapSubject;
    std::map<std::string, PlcIoServer> m_mapSubject_plcIoServer; // 每个TAG只保留最新注册的PLC IO服务器连接

public:  
    CSubscribe() {};  
    ~CSubscribe() {};  

    // 增加订阅者  
    // 同一连接重复订阅同一 tag 时去重（连接以 (对象, 实例序号) 区分）：
    //   DEFAULT 以 (连接, eventid) 为键；其余事件（如 POST_DELAY）以 (连接, eventid, eventname, eventarg) 为键
    void Attach(std::string tagname, EventNode observer)
    {  
        std::unique_lock<std::shared_mutex> lock(mutex_rw);  
        auto& nodes = m_mapSubject[tagname];
        observer.eventname[sizeof(observer.eventname) - 1] = '\0';
        for (const EventNode& n : nodes)
        {
            if (n.subscriber != observer.subscriber || n.seq != observer.seq || n.eventid != observer.eventid)
                continue;
            if (n.eventid == EVENTID::DEFAULT ||
                (n.eventarg == observer.eventarg && strncmp(n.eventname, observer.eventname, sizeof(n.eventname)) == 0))
                return;
        }
        nodes.push_back(observer);
    }

    void Attach(std::string tagname, void* observer, uint64_t seq)
    {  
        EventNode node{ observer, seq, "", EVENTID::DEFAULT, 0 };
        Attach(std::move(tagname), node);
    }

    // 移除订阅者（该连接对象的所有实例）
    void Detach(std::string tagname, void* observer)
    {  
        std::unique_lock<std::shared_mutex> lock(mutex_rw);  
        for (std::list<EventNode>::iterator it = m_mapSubject[tagname].begin(); it != m_mapSubject[tagname].end();)  
        {  
            if ((*it).subscriber == observer)  
                it = m_mapSubject[tagname].erase(it);  
            else  
                ++it;  
        }  
    }

    // 移除已断开的连接实例留下的订阅（投递时发现序号不符）
    void DetachStale(const std::string& tagname, void* observer, uint64_t seq)
    {
        std::unique_lock<std::shared_mutex> lock(mutex_rw);
        auto it = m_mapSubject.find(tagname);
        if (it == m_mapSubject.end())
            return;
        it->second.remove_if([&](const EventNode& n) { return n.subscriber == observer && n.seq == seq; });
    }

    // 查询订阅者  
    std::list<EventNode> GetSubscriber(const std::string& tagname)
    {  
        std::shared_lock<std::shared_mutex> lock(mutex_rw);  
        auto it = m_mapSubject.find(tagname);
        return it == m_mapSubject.end() ? std::list<EventNode>{} : it->second;
    }

    //------------------------------------------------------------------
    // 以下是针对PLC IO服务器的订阅关系维护
    // 增加PLC IO服务器订阅者
    void AttachPlcIoServer(std::string tagname, void* observer, uint64_t seq)
    {  
        std::unique_lock<std::shared_mutex> lock(mutex_rw);  
        m_mapSubject_plcIoServer[tagname] = PlcIoServer{ observer, seq };
    }

    // 移除PLC IO服务器订阅者
    void DetachPlcIoServer(std::string tagname, void* observer)
    {  
        std::unique_lock<std::shared_mutex> lock(mutex_rw);  
        auto it = m_mapSubject_plcIoServer.find(tagname);
        if (it != m_mapSubject_plcIoServer.end() && it->second.subscriber == observer)
            m_mapSubject_plcIoServer.erase(it);
    }

    // 移除已断开的连接实例留下的注册（投递时发现序号不符）
    void DetachStalePlcIoServer(const std::string& tagname, void* observer, uint64_t seq)
    {
        std::unique_lock<std::shared_mutex> lock(mutex_rw);
        auto it = m_mapSubject_plcIoServer.find(tagname);
        if (it != m_mapSubject_plcIoServer.end() && it->second.subscriber == observer && it->second.seq == seq)
            m_mapSubject_plcIoServer.erase(it);
    }

    // 查询PLC IO服务器订阅者；没有时 subscriber 为 nullptr
    PlcIoServer GetPlcIoServer(const std::string& tagname)
    {  
        std::shared_lock<std::shared_mutex> lock(mutex_rw);  
        auto it = m_mapSubject_plcIoServer.find(tagname);
        return it == m_mapSubject_plcIoServer.end() ? PlcIoServer{} : it->second;
    }
};
