// s7ioserver 日志模块
// 参考 gplat/ngx_log.cxx 设计，使用标准C函数实现

#include "s7log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

// 全局日志对象
s7log_t g_s7log;

// 等级名称
static const char* s7log_level_names[] = {
    "ERROR",
    "WARN",
    "INFO",
    "DEBUG"
};

// ---- 日志轮转（调用前必须持有 log_mutex） ----

static void s7log_rotate()
{
    if (g_s7log.log_rotate_count <= 0)
        return;

    struct stat st;
    if (fstat(g_s7log.fd, &st) == -1)
        return;

    if (st.st_size < g_s7log.log_rotate_size)
        return;

    char oldpath[300];
    char newpath[300];

    // 删除最老的备份 .N
    snprintf(oldpath, sizeof(oldpath), "%s.%d", g_s7log.log_path, g_s7log.log_rotate_count);
    unlink(oldpath);

    // 依次重命名 .N-1 -> .N, ... .1 -> .2
    for (int i = g_s7log.log_rotate_count - 1; i >= 1; i--) {
        snprintf(oldpath, sizeof(oldpath), "%s.%d", g_s7log.log_path, i);
        snprintf(newpath, sizeof(newpath), "%s.%d", g_s7log.log_path, i + 1);
        rename(oldpath, newpath);
    }

    // 当前日志文件 -> .1
    snprintf(newpath, sizeof(newpath), "%s.1", g_s7log.log_path);
    rename(g_s7log.log_path, newpath);

    // 关闭旧fd，打开新的日志文件
    close(g_s7log.fd);
    g_s7log.fd = open(g_s7log.log_path, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (g_s7log.fd == -1) {
        g_s7log.fd = STDERR_FILENO;
    }
}

// ---- 日志写入 ----

void s7log_write(int level, const char* fmt, ...)
{
    if (level > g_s7log.log_level)
        return;

    char buf[2048];
    int pos = 0;
    int remain = sizeof(buf) - 2; // 预留 '\n' 和 '\0'

    // 时间戳
    struct timeval tv;
    struct tm tm;
    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm);

    pos += snprintf(buf + pos, remain - pos,
                    "%4d/%02d/%02d %02d:%02d:%02d",
                    tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                    tm.tm_hour, tm.tm_min, tm.tm_sec);

    // 等级
    const char* level_str = (level >= 0 && level <= S7LOG_DEBUG)
                            ? s7log_level_names[level] : "???";
    pos += snprintf(buf + pos, remain - pos, " [%s] ", level_str);

    // 进程ID
    pos += snprintf(buf + pos, remain - pos, "%d: ", (int)getpid());

    // 用户消息
    va_list args;
    va_start(args, fmt);
    pos += vsnprintf(buf + pos, remain - pos, fmt, args);
    va_end(args);

    // 确保不越界
    if (pos > remain)
        pos = remain;

    buf[pos++] = '\n';
    buf[pos] = '\0';

    // 加锁：保护轮转检查+写入的原子性
    pthread_mutex_lock(&g_s7log.log_mutex);

    // 检查是否需要轮转
    s7log_rotate();

    // 写日志文件
    write(g_s7log.fd, buf, pos);

    pthread_mutex_unlock(&g_s7log.log_mutex);

    // ERROR和WARN同时输出到stderr（如果fd不是stderr）
    if (level <= S7LOG_WARN && g_s7log.fd != STDERR_FILENO) {
        write(STDERR_FILENO, buf, pos);
    }
}

// ---- 初始化 ----

void s7log_init(const char* log_path, int log_level,
                int rotate_count, int rotate_size_mb)
{
    memset(&g_s7log, 0, sizeof(g_s7log));

    g_s7log.log_level = log_level;
    g_s7log.log_rotate_count = rotate_count;
    g_s7log.log_rotate_size = (off_t)rotate_size_mb * 1024 * 1024;

    // 保存日志文件路径
    strncpy(g_s7log.log_path, log_path, sizeof(g_s7log.log_path) - 1);

    // 初始化互斥量
    pthread_mutex_init(&g_s7log.log_mutex, NULL);

    // 打开日志文件（只写|追加|不存在则创建）
    g_s7log.fd = open(log_path, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (g_s7log.fd == -1) {
        fprintf(stderr, "s7log_init: cannot open log file '%s', using stderr\n", log_path);
        g_s7log.fd = STDERR_FILENO;
    }
}

// ---- 关闭 ----

void s7log_close()
{
    if (g_s7log.fd > STDERR_FILENO) {
        close(g_s7log.fd);
        g_s7log.fd = -1;
    }
    pthread_mutex_destroy(&g_s7log.log_mutex);
}
