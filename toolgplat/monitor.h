#ifndef MONITOR_H_
#define MONITOR_H_

#include <string>
#include <vector>

// 显示 tag 当前值后，在到 host:port 的独立连接上订阅并持续显示推送的新值，按 q 退出
void MonitorTags(int conn, const std::string& host, int port, const std::vector<std::string>& tagNames);

#endif // MONITOR_H_
