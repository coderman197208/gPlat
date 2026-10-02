// main.cpp —— 测试套件入口（CLI 解析 → Runner）
//
// 关键隔离前提（务必遵守）：
//   套件自建 mkdtemp sandbox 并自启隔离 gplat（端口默认 18777 起自动探测），
//   绝不触碰 8777 上的生产实例或仓库 qbdfile/。详见 server_fixture.h。
//
// ASan 说明：
//   本可执行在 `make ASAN=1` 下也会被 ASan 插桩。但我们只关心“被 exec 的 gplat 子进程”的泄漏，
//   不关心本 harness 自身（它会 fork、持有 sandbox 字符串等，退出时的“泄漏”是噪声）。
//   因此用弱符号 __asan_default_options 关掉本进程的 leak 检测，并清掉继承来的 ASAN_OPTIONS；
//   真正需要检测的子进程由 ServerFixture 在 fork 后、exec 前单独 setenv ASAN_OPTIONS。
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <cerrno>
#include <sys/prctl.h>
#include <sys/resource.h>

#include "framework/ts.h"

// 关闭 harness 自身的 LeakSanitizer（仅在 ASan 构建下生效；非 ASan 构建为未调用的普通函数）。
extern "C" const char* __asan_default_options()
{
    return "detect_leaks=1:abort_on_error=0:halt_on_error=1";
}

namespace {

void print_usage(const char* argv0)
{
    printf(
        "gPlat 自动化测试套件\n"
        "用法: %s [选项]\n\n"
        "筛选:\n"
        "  -f, --filter <子串>      只跑名字含该子串的用例\n"
        "  -t, --tag <名>           只跑含该标签的用例（可重复，并集）\n"
        "  -x, --exclude-tag <名>   排除含该标签的用例（可重复，优先于 --tag）\n"
        "  -l, --list               只列出匹配用例，不执行\n"
        "门控开关:\n"
        "      --run-destructive    允许破坏性用例（默认隐藏）\n"
        "      --run-stress         允许压力/基准用例（默认隐藏）\n"
        "      --all                等价于同时打开上面两项\n"
        "运行参数:\n"
        "      --seed <u64>         随机种子（默认按时间；用于复现）\n"
        "      --scale <n>          规模系数（压力/churn 放大，默认 1）\n"
        "      --expect-asan        期望运行于 ASan 构建：启用泄漏判定\n"
        "      --gplat <path>       指定 gplat 可执行路径（默认取本程序同目录）\n"
        "  -h, --help               显示本帮助\n\n"
        "标签: board queue string pubsub getresp persist memory churn conn\n"
        "      stress protocol destructive asan whitebox\n",
        argv0);
}

// 取下一个参数值；缺失则报错退出。
const char* need_value(int argc, char** argv, int& i, const char* flag)
{
    if (i + 1 >= argc) {
        fprintf(stderr, "错误: 选项 %s 需要一个参数\n", flag);
        exit(2);
    }
    return argv[++i];
}

// 把标签名并入掩码；未知名字报错退出。
void add_tag(uint32_t& mask, const char* name, const char* flag)
{
    uint32_t bit = ts::tag_from_name(name);
    if (bit == 0) {
        fprintf(stderr, "错误: %s 未知标签 \"%s\"\n", flag, name);
        exit(2);
    }
    mask |= bit;
}

uint64_t parse_number(const char* text, const char* flag)
{
    char* end = nullptr;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 0);
    if (!text[0] || text[0] == '-' || errno || !end || *end) {
        fprintf(stderr, "错误: %s 无效数值 \"%s\"\n", flag, text);
        exit(2);
    }
    return value;
}

}  // namespace

int main(int argc, char** argv)
{
    // 清掉继承来的 ASAN_OPTIONS，避免影响 harness 自身；子进程选项由 ServerFixture 单独注入。
    unsetenv("ASAN_OPTIONS");
    unsetenv("LSAN_OPTIONS");

    // SIGKILL master 后领养本套件的 worker，按具体 PID 回收，避免留下孤儿/僵尸。
    if (prctl(PR_SET_CHILD_SUBREAPER, 1) != 0) {
        perror("PR_SET_CHILD_SUBREAPER");
        return 2;
    }
    rlimit core{0, 0};
    if (setrlimit(RLIMIT_CORE, &core) != 0) {
        perror("disable test core dumps");
        return 2;
    }

    ts::RunOptions opt;

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (!strcmp(a, "-f") || !strcmp(a, "--filter")) {
            opt.filter = need_value(argc, argv, i, a);
        } else if (!strcmp(a, "-t") || !strcmp(a, "--tag")) {
            add_tag(opt.tagMask, need_value(argc, argv, i, a), a);
        } else if (!strcmp(a, "-x") || !strcmp(a, "--exclude-tag")) {
            add_tag(opt.excludeMask, need_value(argc, argv, i, a), a);
        } else if (!strcmp(a, "-l") || !strcmp(a, "--list")) {
            opt.list = true;
        } else if (!strcmp(a, "--run-destructive")) {
            opt.runDestructive = true;
        } else if (!strcmp(a, "--run-stress")) {
            opt.runStress = true;
        } else if (!strcmp(a, "--all")) {
            opt.all = true;
        } else if (!strcmp(a, "--seed")) {
            opt.seed = parse_number(need_value(argc, argv, i, a), a);
            opt.seedSet = true;
        } else if (!strcmp(a, "--scale")) {
            uint64_t scale = parse_number(need_value(argc, argv, i, a), a);
            if (scale < 1 || scale > 64) {
                fprintf(stderr, "错误: --scale 必须在 1..64 之间\n");
                return 2;
            }
            opt.scale = static_cast<int>(scale);
        } else if (!strcmp(a, "--expect-asan")) {
            opt.expectAsan = true;
        } else if (!strcmp(a, "--gplat")) {
            opt.gplatPath = need_value(argc, argv, i, a);
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "错误: 未知选项 \"%s\"（--help 查看用法）\n", a);
            return 2;
        }
    }

#ifndef __SANITIZE_ADDRESS__
    if (opt.expectAsan) {
        fprintf(stderr, "--expect-asan requires make ASAN=1 testsuite\n");
        return 2;
    }
#endif

    ts::Runner runner(opt);
    return runner.run();
}
