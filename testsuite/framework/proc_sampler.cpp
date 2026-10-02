// proc_sampler.cpp —— 见 proc_sampler.h
#include "proc_sampler.h"

#include <dirent.h>

#include <cstdio>
#include <cstring>
#include <ctime>

namespace ts {

static uint64_t mono_nanos()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// 读 /proc/<pid>/status 的 VmRSS / VmHWM（单位 KB）。
static void read_status(pid_t pid, long& rss, long& hwm)
{
    rss = -1;
    hwm = -1;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (rss < 0 && strncmp(line, "VmRSS:", 6) == 0)
            sscanf(line + 6, "%ld", &rss);
        else if (hwm < 0 && strncmp(line, "VmHWM:", 6) == 0)
            sscanf(line + 6, "%ld", &hwm);
        if (rss >= 0 && hwm >= 0) break;
    }
    fclose(f);
}

// 数 /proc/<pid>/fd 目录条目数（不含 . 和 ..）。
static int count_fds(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/fd", (int)pid);
    DIR* d = opendir(path);
    if (!d) return -1;
    int n = 0;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] == '.' &&
            (e->d_name[1] == '\0' || (e->d_name[1] == '.' && e->d_name[2] == '\0')))
            continue;
        n++;
    }
    closedir(d);
    return n;
}

ProcSample sample_proc(pid_t pid)
{
    ProcSample s;
    s.t_nanos = mono_nanos();
    read_status(pid, s.rss_kb, s.hwm_kb);
    s.fd_count = count_fds(pid);
    return s;
}

bool ProcSampler::tick()
{
    ProcSample s = sample_proc(pid_);
    if (!s.valid()) return false;
    samples_.push_back(s);
    return true;
}

void ProcSampler::drop_front(size_t n)
{
    if (n >= samples_.size())
        samples_.clear();
    else
        samples_.erase(samples_.begin(), samples_.begin() + n);
}

// 最小二乘斜率：y 对 t(秒) 回归。样本不足 2 返回 0。
double ProcSampler::slope(bool fd) const
{
    size_t n = samples_.size();
    if (n < 2) return 0.0;
    double t0 = (double)samples_.front().t_nanos;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (const auto& s : samples_) {
        double x = ((double)s.t_nanos - t0) / 1e9;  // 秒
        double y = fd ? (double)s.fd_count : (double)s.rss_kb;
        sx += x; sy += y; sxx += x * x; sxy += x * y;
    }
    double denom = n * sxx - sx * sx;
    if (denom == 0.0) return 0.0;
    return (n * sxy - sx * sy) / denom;
}

double ProcSampler::rss_slope_kb_per_s() const { return slope(false); }
double ProcSampler::fd_slope_per_s() const { return slope(true); }

long ProcSampler::rss_first_kb() const { return samples_.empty() ? -1 : samples_.front().rss_kb; }
long ProcSampler::rss_last_kb() const { return samples_.empty() ? -1 : samples_.back().rss_kb; }

long ProcSampler::rss_peak_kb() const
{
    long m = -1;
    for (const auto& s : samples_) if (s.hwm_kb > m) m = s.hwm_kb;
    return m;
}

int ProcSampler::fd_first() const { return samples_.empty() ? -1 : samples_.front().fd_count; }
int ProcSampler::fd_last() const { return samples_.empty() ? -1 : samples_.back().fd_count; }

int ProcSampler::fd_max() const
{
    int m = -1;
    for (const auto& s : samples_) if (s.fd_count > m) m = s.fd_count;
    return m;
}

void ProcSampler::report(const char* label) const
{
    printf("    %-24s samples=%zu  RSS %ld->%ld KB (peak %ld, slope %.1f KB/s)  fd %d->%d (max %d, slope %.3f /s)\n",
           label, samples_.size(),
           rss_first_kb(), rss_last_kb(), rss_peak_kb(), rss_slope_kb_per_s(),
           fd_first(), fd_last(), fd_max(), fd_slope_per_s());
}

}  // namespace ts
