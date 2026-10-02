// workload.h —— 可复现随机负载工具：带种子 PRNG、CRC32、延迟直方图
//
// 设计动机（对应方案第 0/10 节）：
//   * 生产环境读写是随机且复杂的 → 用带种子的 PRNG 生成负载；失败时打印种子即可精确重放。
//   * 数据完整性 → 每条负载附带序列号与 CRC32，读回时校验，能发现错位/交错/损坏。
//   * 性能仅作报告 → 延迟直方图给出 p50/p99/p999、min/max/mean、吞吐，不设绝对阈值。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ts {

// ---------------------------------------------------------------------------
// 确定性 PRNG（splitmix64 → xorshift128+ 风格），同种子同序列，跨平台一致。
// ---------------------------------------------------------------------------
class Rng {
public:
    explicit Rng(uint64_t seed) { reseed(seed); }

    void reseed(uint64_t seed)
    {
        // splitmix64 预热，产生两个非零状态字
        s_[0] = splitmix(seed);
        s_[1] = splitmix(seed);
        if ((s_[0] | s_[1]) == 0) s_[0] = 0x9E3779B97F4A7C15ull;
    }

    uint64_t next_u64()
    {
        uint64_t x = s_[0];
        const uint64_t y = s_[1];
        s_[0] = y;
        x ^= x << 23;
        s_[1] = x ^ y ^ (x >> 17) ^ (y >> 26);
        return s_[1] + y;
    }

    uint32_t next_u32() { return (uint32_t)(next_u64() >> 32); }

    // 闭区间 [lo, hi] 内的均匀整数（hi>=lo）。
    int range(int lo, int hi)
    {
        if (hi <= lo) return lo;
        uint64_t span = (uint64_t)(hi - lo) + 1;
        return lo + (int)(next_u64() % span);
    }

    bool chance(int percent) { return range(1, 100) <= percent; }

    // 用确定性字节填充缓冲区。
    void fill(void* buf, size_t len)
    {
        uint8_t* p = (uint8_t*)buf;
        for (size_t i = 0; i < len; i++) p[i] = (uint8_t)next_u32();
    }

private:
    static uint64_t splitmix(uint64_t& x)
    {
        uint64_t z = (x += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    uint64_t s_[2];
};

// ---------------------------------------------------------------------------
// CRC32（IEEE 802.3，多项式 0xEDB88320），用于数据完整性校验。
// ---------------------------------------------------------------------------
uint32_t crc32(const void* data, size_t len, uint32_t seed = 0);

// ---------------------------------------------------------------------------
// 负载记录头：写进 payload 前若干字节，读回时自校验（序列号 + 载荷 CRC）。
// 布局与端序固定，便于跨连接/跨进程比对。
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct PayloadTag {
    uint32_t magic;    // 0xB0A4D000 识别
    uint32_t seq;      // 递增序列号
    uint32_t len;      // 载荷总长（含本头）
    uint32_t crc;      // 对 [sizeof(PayloadTag), len) 的 CRC32
};
#pragma pack(pop)

// 用 rng 生成一条带自校验头的 payload（总长 total>=sizeof(PayloadTag)）。
void make_payload(std::vector<char>& out, int total, uint32_t seq, Rng& rng);
// 校验一条 payload 的头与 CRC；expectSeq<0 表示不校验序列号。返回 true=完好。
bool check_payload(const void* data, int len, long expectSeq, std::string* err);

// ---------------------------------------------------------------------------
// 延迟直方图：收集纳秒样本，给出分位数与吞吐。
// ---------------------------------------------------------------------------
class LatencyHistogram {
public:
    void record(uint64_t nanos) { samples_.push_back(nanos); sorted_ = false; }
    void reserve(size_t n) { samples_.reserve(n); }
    size_t count() const { return samples_.size(); }
    const std::vector<uint64_t>& samples() const { return samples_; }

    // 计算分位数（百分位 0..100，支持 99.9 用 pct=99.9）。单位：微秒。
    double percentile_us(double pct);
    double min_us();
    double max_us();
    double mean_us();

    // 打印一行统计：<label>: n=.. p50=.. p99=.. p999=.. max=.. (us)
    void report(const char* label, double wallSeconds = 0.0);

private:
    void ensure_sorted();
    std::vector<uint64_t> samples_;
    bool sorted_ = false;
};

// 单调时钟计时（纳秒）。
uint64_t now_nanos();

}  // namespace ts
