#include <cmath>
#include <thread>
#include "framework/ts.h"

using namespace ts;

TEST("framework.workload_selfcheck", TAG_BOARD)
{
    CHECK(ctx, crc32("123456789", 9) == 0xcbf43926u, "CRC32 reference vector mismatch");
    Rng a(ctx.seed), b(ctx.seed);
    for (int i = 0; i < 64; ++i)
        CHECK(ctx, a.next_u64() == b.next_u64(), "PRNG replay differs at %d", i);
    std::vector<char> payload;
    make_payload(payload, 64, 42, a);
    std::string error;
    CHECK(ctx, check_payload(payload.data(), 64, 42, &error), "%s", error.c_str());
    payload.back() ^= 1;
    CHECK(ctx, !check_payload(payload.data(), 64, 42, &error), "CRC did not detect corruption");
    LatencyHistogram h;
    h.record(3000);
    CHECK(ctx, h.percentile_us(100) == 3, "histogram initial value");
    h.record(1000);
    CHECK(ctx, h.percentile_us(0) == 1 && h.percentile_us(100) == 3,
          "histogram did not invalidate sorted state on append");
}

TEST("framework.fork_watchdog", TAG_BOARD)
{
    ForkResult ok = run_in_fork([] {});
    CHECK(ctx, ok.exited && ok.exit_code == 0, "fork: %s", ok.describe());
    ForkResult timeout = run_in_fork([] { std::this_thread::sleep_for(std::chrono::seconds(1)); }, 20);
    CHECK(ctx, timeout.timed_out, "watchdog did not terminate timed-out child");
}
