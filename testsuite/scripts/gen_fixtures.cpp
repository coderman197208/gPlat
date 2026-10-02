// gen_fixtures.cpp —— 独立的夹具生成器（复用 framework/fixtures.o）
//
// 用途：手工把测试所需的 BOARD/QUEUE 通过“本地 API”灌入指定 qbd 目录，
//   供离线准备 sandbox、或 scripts/fixtures.sh 调用。套件自身启动时会在 forked 子进程里
//   直接调用 create_all_fixtures，并不依赖本程序；本程序是面向人工/脚本的便捷入口。
//
// 它会改写本进程全局的 dataQuePath/已加载表 —— 但作为一次性独立进程无所谓。
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "framework/fixtures.h"

static void usage(const char* a0)
{
    printf("用法: %s <qbd目录> [--scale N]\n"
           "  在 <qbd目录> 下用本地 API 建好标准 BOARD/QUEUE 夹具。\n"
           "  <qbd目录> 必须已存在。返回失败项数（0=全部成功）。\n", a0);
}

int main(int argc, char** argv)
{
    const char* qbd = nullptr;
    int scale = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scale") && i + 1 < argc) {
            scale = atoi(argv[++i]);
            if (scale < 1) scale = 1;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-' && !qbd) {
            qbd = argv[i];
        } else {
            fprintf(stderr, "未知参数: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }
    if (!qbd) { usage(argv[0]); return 2; }

    printf("[gen_fixtures] qbd=%s scale=%d\n", qbd, scale);
    int failures = ts::create_all_fixtures(qbd, scale);
    if (failures == 0)
        printf("[gen_fixtures] 全部夹具创建成功\n");
    else
        fprintf(stderr, "[gen_fixtures] %d 项夹具创建失败\n", failures);
    return failures;
}
