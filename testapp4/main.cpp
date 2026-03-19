#include <cstdio>
#include <chrono>
#include <thread>       //std::this_thread::sleep_for   std::chrono::milliseconds
#include <atomic>
#include <unistd.h>     //sleep(1)
#include <signal.h>
#include <iostream>

#include "../include/hello.h"
#include "../include/higplat.h"
#include "../include/qbdtype.h"
#include <string.h>

std::atomic<bool> g_running(true);  // 控制线程运行的标志

void threadFunction1();
void threadFunction2();
void threadFunction3();

int main()
{
    // 创建并启动线程
    std::thread worker1(threadFunction1);
    //std::thread worker2(threadFunction2);
    //std::thread worker3(threadFunction3);


    // 主线程可以继续做其他工作...
    // 主线程等待键盘输入
    std::string input;
    while (true) {
        std::cin >> input;  // 等待用户输入
        if (input == "q") {
            g_running = false;  // 通知线程退出
            std::cout << "exiting..." << std::endl;
            break;
        }
        else {
            std::cout << "unknown command，input 'q' exit" << std::endl;
        }
    }

    // 如果需要等待线程完成（虽然这个线程是无限循环）
    worker1.join();
	//worker2.join();
    //worker3.join();

    std::cout << "main thread exit" << std::endl;

    return 0;
}
