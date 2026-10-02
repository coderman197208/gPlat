// proc_sampler.h —— /proc 资源采样与趋势判定
//
// 用途（对应方案第 5 节“趋势法”）：LSan 看不见“仍可达但无界增长”的泄漏
//   （连接池无界、m_listPost/m_mapResponseOwner 堆积、fd 泄漏）。
//   周期采样 worker 的 VmRSS/VmHWM 与打开的 fd 数，对稳态段做线性回归求斜率；
//   稳态应 ≈ 0，持续单调上升即判为增长趋势（多为 XFAIL 登记项，只报告）。
#pragma once

#include <sys/types.h>

#include <cstdint>
#include <vector>

namespace ts {

struct ProcSample {
    uint64_t t_nanos = 0;  // 采样时刻（单调钟）
    long rss_kb = -1;     // VmRSS
    long hwm_kb = -1;     // VmHWM（峰值 RSS）
    int  fd_count = -1;   // /proc/<pid>/fd 条目数
    bool valid() const { return rss_kb >= 0; }
};

// 单次采样某进程；进程不存在或无权限 → valid()==false。
ProcSample sample_proc(pid_t pid);

// 采样序列 + 回归。记录一段时间内的多次采样，给出斜率与极值。
class ProcSampler {
public:
    explicit ProcSampler(pid_t pid) : pid_(pid) {}

    // 立即采一次并追加。返回是否成功。
    bool tick();

    size_t count() const { return samples_.size(); }
    const std::vector<ProcSample>& samples() const { return samples_; }

    // 丢弃前 n 个样本（去掉预热/爬升段，只看稳态）。
    void drop_front(size_t n);

    // RSS 斜率（KB/秒）与 fd 斜率（个/秒），对当前全部样本做最小二乘。
    double rss_slope_kb_per_s() const;
    double fd_slope_per_s() const;

    long rss_first_kb() const;
    long rss_last_kb() const;
    long rss_peak_kb() const;  // 取各样本 hwm 的最大值
    int  fd_first() const;
    int  fd_last() const;
    int  fd_max() const;

    // 打印一行趋势报告。
    void report(const char* label) const;

private:
    double slope(bool fd) const;  // true=fd, false=rss
    pid_t pid_;
    std::vector<ProcSample> samples_;
};

}  // namespace ts
