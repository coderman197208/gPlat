#pragma once  
#include <list>  
#include <map>  
#include <string>  
#include <shared_mutex>  
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
    std::map<std::string, std::list<void*>> m_mapSubject_plcIoServer; //保存TAG对应的IO服务器列表 void*(lpngx_connection_t)

public:  
    CSubscribe() {};  
    ~CSubscribe() {};  

    // 增加订阅者  
    void Attach(std::string tagname, EventNode observer)
    {  
        std::unique_lock<std::shared_mutex> lock(mutex_rw);  
        m_mapSubject[tagname].push_back(observer);  
    }

    void Attach(std::string tagname, void* observer)
    {  
        std::unique_lock<std::shared_mutex> lock(mutex_rw);  
        m_mapSubject[tagname].push_back(EventNode{ observer,"",EVENTID::DEFAULT,0 });  
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
        m_mapSubject_plcIoServer[tagname].push_back(observer);  
    }

    // 移除PLC IO服务器订阅者
    void DetachPlcIoServer(std::string tagname, void* observer)
    {  
        std::unique_lock<std::shared_mutex> lock(mutex_rw);  
        for (std::list<void*>::iterator it = m_mapSubject_plcIoServer[tagname].begin(); it != m_mapSubject_plcIoServer[tagname].end();)  
        {  
            if ((*it) == observer)  
                it = m_mapSubject_plcIoServer[tagname].erase(it);  
            else  
                ++it;  
        }  
    }

    // 查询PLC IO服务器订阅者
    std::list<void*> GetPlcIoServer(const std::string& tagname)
    {  
        std::shared_lock<std::shared_mutex> lock(mutex_rw);  
        auto it = m_mapSubject_plcIoServer.find(tagname);
        return it == m_mapSubject_plcIoServer.end() ? std::list<void*>{} : it->second;
    }
};
