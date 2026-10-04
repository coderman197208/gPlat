#pragma once  
#include <list>  
#include <map>  
#include <string>  
#include <shared_mutex>  
#include <cstring>
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
    char      eventname[40];  
    EVENTID   eventid;  
    int       eventarg;  
};  

class CSubscribe  
{  
private:  
    std::shared_mutex mutex_rw;  
    std::map<std::string, std::list<EventNode>> m_mapSubject;
    std::map<std::string, void*> m_mapSubject_plcIoServer; // 每个TAG只保留最新注册的PLC IO服务器连接

public:  
    CSubscribe() {};  
    ~CSubscribe() {};  

    // 增加订阅者  
    // 同一连接重复订阅同一 tag 时去重：
    //   DEFAULT 以 (连接, eventid) 为键；其余事件（如 POST_DELAY）以 (连接, eventid, eventname, eventarg) 为键
    void Attach(std::string tagname, EventNode observer)
    {  
        std::unique_lock<std::shared_mutex> lock(mutex_rw);  
        auto& nodes = m_mapSubject[tagname];
        observer.eventname[sizeof(observer.eventname) - 1] = '\0';
        for (const EventNode& n : nodes)
        {
            if (n.subscriber != observer.subscriber || n.eventid != observer.eventid)
                continue;
            if (n.eventid == EVENTID::DEFAULT ||
                (n.eventarg == observer.eventarg && strncmp(n.eventname, observer.eventname, sizeof(n.eventname)) == 0))
                return;
        }
        nodes.push_back(observer);
    }

    void Attach(std::string tagname, void* observer)
    {  
        EventNode node{ observer,"",EVENTID::DEFAULT,0 };
        Attach(std::move(tagname), node);
    }

    // 移除订阅者  
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
    void AttachPlcIoServer(std::string tagname, void* observer)
    {  
        std::unique_lock<std::shared_mutex> lock(mutex_rw);  
        m_mapSubject_plcIoServer[tagname] = observer;
    }

    // 移除PLC IO服务器订阅者
    void DetachPlcIoServer(std::string tagname, void* observer)
    {  
        std::unique_lock<std::shared_mutex> lock(mutex_rw);  
        auto it = m_mapSubject_plcIoServer.find(tagname);
        if (it != m_mapSubject_plcIoServer.end() && it->second == observer)
            m_mapSubject_plcIoServer.erase(it);
    }

    // 查询PLC IO服务器订阅者
    void* GetPlcIoServer(const std::string& tagname)
    {  
        std::shared_lock<std::shared_mutex> lock(mutex_rw);  
        auto it = m_mapSubject_plcIoServer.find(tagname);
        return it == m_mapSubject_plcIoServer.end() ? nullptr : it->second;
    }
};
