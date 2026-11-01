#ifndef __NGX_FUNC_H__
#define __NGX_FUNC_H__

//字符串相关函数
void   Rtrim(char *string);
void   Ltrim(char *string);

//和日志，打印输出有关
void   ngx_log_init();
void   ngx_log_stderr(int err, const char *fmt, ...);
void   ngx_log_error_core(int level,  int err, const char *fmt, ...);
u_char *ngx_log_errno(u_char *buf, u_char *last, int err);
u_char *ngx_snprintf(u_char *buf, size_t max, const char *fmt, ...);
u_char *ngx_slprintf(u_char *buf, u_char *last, const char *fmt, ...);
u_char *ngx_vslprintf(u_char *buf, u_char *last,const char *fmt,va_list args);

//和信号/主流程相关相关
int    ngx_init_signals();           //屏蔽并用signalfd接收SIGINT/SIGTERM等，必须在创建线程之前调用
void   ngx_handle_signals();         //signalfd可读时由epoll线程调用
void   ngx_close_signals();
void   ngx_single_process_cycle();
int    ngx_daemon();
void   ngx_process_events_and_timers();

#endif  