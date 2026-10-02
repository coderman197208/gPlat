// workload.cpp —— 见 workload.h
#include "workload.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <array>

namespace ts {

// ---- CRC32 -----------------------------------------------------------------
static const std::array<uint32_t, 256>& crc_table()
{
    static const auto table = [] {
        std::array<uint32_t, 256> out{};
        for (uint32_t i = 0; i < out.size(); i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            out[i] = c;
        }
        return out;
    }();
    return table;
}

uint32_t crc32(const void* data, size_t len, uint32_t seed)
{
    const auto& table = crc_table();
    uint32_t c = seed ^ 0xFFFFFFFFu;
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++)
        c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// ---- 自校验 payload --------------------------------------------------------
static const uint32_t kPayloadMagic = 0xB0A4D000u;

void make_payload(std::vector<char>& out, int total, uint32_t seq, Rng& rng)
{
    if (total < (int)sizeof(PayloadTag)) total = (int)sizeof(PayloadTag);
    out.resize(total);
    // 先填随机载荷区，再回填头（CRC 覆盖载荷区）
    char* body = out.data() + sizeof(PayloadTag);
    int bodyLen = total - (int)sizeof(PayloadTag);
    rng.fill(body, bodyLen);

    PayloadTag tag;
    tag.magic = kPayloadMagic;
    tag.seq = seq;
    tag.len = (uint32_t)total;
    tag.crc = crc32(body, bodyLen);
    memcpy(out.data(), &tag, sizeof(tag));
}

bool check_payload(const void* data, int len, long expectSeq, std::string* err)
{
    char msg[160];
    if (len < (int)sizeof(PayloadTag)) {
        if (err) *err = "payload shorter than header";
        return false;
    }
    PayloadTag tag;
    memcpy(&tag, data, sizeof(tag));
    if (tag.magic != kPayloadMagic) {
        if (err) { snprintf(msg, sizeof(msg), "bad magic 0x%08X", tag.magic); *err = msg; }
        return false;
    }
    if ((int)tag.len != len) {
        if (err) { snprintf(msg, sizeof(msg), "len mismatch: header=%u actual=%d", tag.len, len); *err = msg; }
        return false;
    }
    if (expectSeq >= 0 && tag.seq != (uint32_t)expectSeq) {
        if (err) { snprintf(msg, sizeof(msg), "seq mismatch: want %ld got %u", expectSeq, tag.seq); *err = msg; }
        return false;
    }
    const char* body = (const char*)data + sizeof(PayloadTag);
    uint32_t crc = crc32(body, len - (int)sizeof(PayloadTag));
    if (crc != tag.crc) {
        if (err) { snprintf(msg, sizeof(msg), "crc mismatch: header=0x%08X actual=0x%08X", tag.crc, crc); *err = msg; }
        return false;
    }
    return true;
}

// ---- 延迟直方图 ------------------------------------------------------------
void LatencyHistogram::ensure_sorted()
{
    if (!sorted_) {
        std::sort(samples_.begin(), samples_.end());
        sorted_ = true;
    }
}

double LatencyHistogram::percentile_us(double pct)
{
    if (samples_.empty()) return 0.0;
    ensure_sorted();
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    // 最近秩法
    double rank = (pct / 100.0) * (samples_.size() - 1);
    size_t idx = (size_t)(rank + 0.5);
    if (idx >= samples_.size()) idx = samples_.size() - 1;
    return samples_[idx] / 1000.0;
}

double LatencyHistogram::min_us()
{
    if (samples_.empty()) return 0.0;
    ensure_sorted();
    return samples_.front() / 1000.0;
}

double LatencyHistogram::max_us()
{
    if (samples_.empty()) return 0.0;
    ensure_sorted();
    return samples_.back() / 1000.0;
}

double LatencyHistogram::mean_us()
{
    if (samples_.empty()) return 0.0;
    double sum = 0;
    for (uint64_t s : samples_) sum += (double)s;
    return sum / samples_.size() / 1000.0;
}

void LatencyHistogram::report(const char* label, double wallSeconds)
{
    if (samples_.empty()) {
        printf("    %-28s (no samples)\n", label);
        return;
    }
    double qps = wallSeconds > 0 ? samples_.size() / wallSeconds : 0.0;
    printf("    %-28s n=%zu  p50=%.1f p90=%.1f p99=%.1f p999=%.1f max=%.1f mean=%.1f us",
           label, samples_.size(),
           percentile_us(50), percentile_us(90), percentile_us(99),
           percentile_us(99.9), max_us(), mean_us());
    if (qps > 0) printf("  | %.0f ops/s", qps);
    printf("\n");
}

uint64_t now_nanos()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

}  // namespace ts
