#include <cstdio>
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>
#include <unistd.h>
#include <signal.h>
#include <iostream>
#include <cstring>

#include "s7config.h"

std::atomic<bool> g_running(true);

// 前向声明
void threadReadPlc(PlcConfig* plc, AppConfig* config);
void threadWritePlc(AppConfig* config);

void signalHandler(int sig) {
    g_running = false;
}

int main(int argc, char* argv[])
{
    // 注册信号处理
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    // 配置文件路径
    std::string configFile = "s7ioserver.ini";
    if (argc > 1) {
        configFile = argv[1];
    }

    // 加载配置
    AppConfig config;
    if (!LoadConfig(configFile, config)) {
        fprintf(stderr, "Failed to load config file: %s\n", configFile.c_str());
        return 1;
    }

    // 打印配置信息
    PrintConfig(config);

    if (config.plcs.empty()) {
        fprintf(stderr, "No PLC configured. Exiting.\n");
        return 1;
    }

    printf("s7ioserver started. %zu PLC(s) configured.\n", config.plcs.size());

    // 每个PLC启动一个读线程
    std::vector<std::thread> readThreads;
    for (auto& plc : config.plcs) {
        readThreads.emplace_back(threadReadPlc, &plc, &config);
    }

	// 不要让读线程和写线程同时尝试连接PLC，先让读线程启动并连接成功，否则可能出现竞争条件导致连接失败。
    sleep(1);

    // 启动一个共享写线程
    std::thread writeThread(threadWritePlc, &config);

    // 主线程等待退出命令或信号
    std::string input;
    while (g_running) {
        if (std::cin >> input) {
            if (input == "q" || input == "quit") {
                g_running = false;
                printf("Shutting down...\n");
                break;
            } else {
                printf("Unknown command. Type 'q' to quit.\n");
            }
        } else {
            // stdin关闭（如以daemon运行），等待信号
            while (g_running) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            break;
        }
    }

    g_running = false;

    // 等待所有线程结束
    for (auto& t : readThreads) {
        if (t.joinable()) t.join();
    }
    if (writeThread.joinable()) writeThread.join();

    printf("s7ioserver exited.\n");
    return 0;
}
