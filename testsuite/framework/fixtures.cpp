// fixtures.cpp —— 见 fixtures.h
//
// 用本地 API（直接 mmap QBD 文件）在 sandbox/qbdfile 下建好标准夹具。
// 约定：类型描述符为 8 字节 [int typecode][int arraysize]（与 toolgplat/testapp 一致）。
// 建 BOARD 的 tag 必须先 LoadQ("BOARD") 把板子加载进本进程（CreateItem 内部 fetchtab 要求）。
#include "fixtures.h"

#include <cstdio>
#include <cstring>

#include "higplat.h"    // 本地 API: SetQbdPath/CreateB/LoadQ/CreateItem/CreateQ
#include "type_code.h"  // TypeCode

namespace ts {

// 8 字节类型描述符。
struct TypeDesc {
    int code;
    int arraysize;
};

static TypeDesc desc(int code, int arraysize) { return TypeDesc{code, arraysize}; }

// 建一个 BOARD tag，失败打印并计数。
static int make_tag(const char* board, const char* name, int itemSize, TypeDesc d, int& failures)
{
    if (!CreateItem(board, name, itemSize, &d, (int)sizeof(d))) {
        fprintf(stderr, "[fixtures] CreateItem(%s,%d) failed\n", name, itemSize);
        failures++;
        return 1;
    }
    return 0;
}

static int make_queue(const char* name, int rec, int num, int dateType, int mode, int& failures)
{
    if (!CreateQ(name, rec, num, dateType, mode, nullptr, 0)) {
        fprintf(stderr, "[fixtures] CreateQ(%s) failed\n", name);
        failures++;
        return 1;
    }
    return 0;
}

int create_all_fixtures(const char* qbdPath, int scale)
{
    if (scale < 1) scale = 1;
    int failures = 0;

    SetQbdPath(qbdPath);

    // ---- BOARD ----
    if (!CreateB(fx::BOARD, fx::BOARD_SIZE)) {
        fprintf(stderr, "[fixtures] CreateB(BOARD) failed\n");
        return failures + 1;  // 没有板子，后续 tag 无从谈起
    }
    if (!LoadQ(fx::BOARD)) {
        fprintf(stderr, "[fixtures] LoadQ(BOARD) failed\n");
        return failures + 1;
    }

    // 标量 / 数组（带类型描述符）
    make_tag(fx::BOARD, fx::TAG_BOOL, 1, desc(Boolean, 0), failures);
    make_tag(fx::BOARD, fx::TAG_I16, 2, desc(Int16, 0), failures);
    make_tag(fx::BOARD, fx::TAG_I32, 4, desc(Int32, 0), failures);
    make_tag(fx::BOARD, fx::TAG_I64, 8, desc(Int64, 0), failures);
    make_tag(fx::BOARD, fx::TAG_F32, 4, desc(Single, 0), failures);
    make_tag(fx::BOARD, fx::TAG_F64, 8, desc(Double, 0), failures);
    make_tag(fx::BOARD, fx::TAG_ARR, 16, desc(Single, 4), failures);

    // 通用二进制（char[N]）
    make_tag(fx::BOARD, fx::TAG_BIN256, 256, desc(Char, 256), failures);
    make_tag(fx::BOARD, fx::TAG_BIN4K, 4096, desc(Char, 4096), failures);
    make_tag(fx::BOARD, fx::TAG_BINMAX, 16384, desc(Char, 16384), failures);
    make_tag(fx::BOARD, fx::TAG_POST, 256, desc(Char, 256), failures);

    // 字符串（String 类型，容量 = itemsize）
    make_tag(fx::BOARD, fx::STR_1, 1, desc(String, 1), failures);
    make_tag(fx::BOARD, fx::STR_16, 16, desc(String, 16), failures);
    make_tag(fx::BOARD, fx::STR_64, 64, desc(String, 64), failures);
    make_tag(fx::BOARD, fx::STR_MAX, 16384, desc(String, 16384), failures);

    // 请求/响应
    make_tag(fx::BOARD, fx::REQ_TAG, 256, desc(Char, 256), failures);
    make_tag(fx::BOARD, fx::RSP_TAG, 256, desc(Char, 256), failures);

    // 持久化专用
    make_tag(fx::BOARD, fx::TAG_PERSIST, 4, desc(Int32, 0), failures);

    // ---- 队列：NORMAL/SHIFT × BINARY/ASCII + 大记录 ----
    make_queue(fx::Q_NORMAL_BIN, fx::Q_RECSIZE, fx::Q_COUNT, BINARY_TYPE, NORMAL_MODE, failures);
    make_queue(fx::Q_SHIFT_BIN, fx::Q_RECSIZE, fx::Q_COUNT, BINARY_TYPE, SHIFT_MODE, failures);
    make_queue(fx::Q_NORMAL_ASC, fx::Q_RECSIZE, fx::Q_COUNT, ASCII_TYPE, NORMAL_MODE, failures);
    make_queue(fx::Q_SHIFT_ASC, fx::Q_RECSIZE, fx::Q_COUNT, ASCII_TYPE, SHIFT_MODE, failures);
    make_queue(fx::Q_BIG_BIN, fx::Q_BIG_RECSIZE, fx::Q_BIG_COUNT, BINARY_TYPE, NORMAL_MODE, failures);

    return failures;
}

}  // namespace ts
