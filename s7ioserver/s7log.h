#ifndef S7LOG_H
#define S7LOG_H

#include <pthread.h>
#include <sys/types.h>

// 日志等级（数字越小等级越高）
#define S7LOG_ERROR   0
#define S7LOG_WARN    1
#define S7LOG_INFO    2
#define S7LOG_DEBUG   3

// 日志配置结构
typedef struct {
    int             log_level;           // 当前日志等级
    int             fd;                  // 日志文件描述符
    char            log_path[256];       // 日志文件路径
    int             log_rotate_count;    // 轮转备份文件数量（0=不轮转）
    off_t           log_rotate_size;     // 单个日志文件最大字节数
    pthread_mutex_t log_mutex;           // 写入互斥量
} s7log_t;

extern s7log_t g_s7log;

// 初始化日志系统（在LoadConfig之后调用）
void s7log_init(const char* log_path, int log_level,
                int rotate_count, int rotate_size_mb);

// 关闭日志系统
void s7log_close();

// 写日志（可变参数），level为日志等级
void s7log_write(int level, const char* fmt, ...)
    __attribute__((format(printf, 2, 3)));

// 便捷宏
#define s7log_error(fmt, ...)  s7log_write(S7LOG_ERROR, fmt, ##__VA_ARGS__)
#define s7log_warn(fmt, ...)   s7log_write(S7LOG_WARN,  fmt, ##__VA_ARGS__)
#define s7log_info(fmt, ...)   s7log_write(S7LOG_INFO,  fmt, ##__VA_ARGS__)
#define s7log_debug(fmt, ...)  s7log_write(S7LOG_DEBUG, fmt, ##__VA_ARGS__)

#endif // S7LOG_H
