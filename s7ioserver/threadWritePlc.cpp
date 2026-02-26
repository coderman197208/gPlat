#include <thread>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <cstring>
#include <chrono>
#include <map>
#include <unistd.h>

#include "../include/higplat.h"
#include "../include/snap7.h"
#include "s7config.h"

extern std::atomic<bool> g_running;

// tag查找信息
struct TagLookup {
    TagConfig* tag;
    PlcConfig* plc;
    S7Object   client;
};

// ---- snap7 重连 (写线程用) ----

static bool reconnectSnap7Write(S7Object client, const PlcConfig& plc, int interval) {
    Cli_Disconnect(client);
    while (g_running) {
        printf("[%s/write] snap7 reconnecting to %s...\n", plc.name.c_str(), plc.ip.c_str());
        int res = Cli_ConnectTo(client, plc.ip.c_str(), plc.rack, plc.slot);
        if (res == 0) {
            printf("[%s/write] snap7 reconnected.\n", plc.name.c_str());
            return true;
        }
        for (int i = 0; i < interval / 100 && g_running; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

// ---- gPlat 重连 + 重新注册 ----

static int reconnectGplatWrite(AppConfig* config, std::map<std::string, TagLookup>& tagMap) {
    while (g_running) {
        printf("[write] gPlat reconnecting to %s:%d...\n",
               config->gplat_server.c_str(), config->gplat_port);
        int conn = connectgplat(config->gplat_server.c_str(), config->gplat_port);
        if (conn > 0) {
            printf("[write] gPlat reconnected (fd=%d), re-registering...\n", conn);
            unsigned int err;

            // 重新订阅timer
            subscribe(conn, "timer_500ms", &err);

            // 重新注册所有tag
            for (auto& kv : tagMap) {
                registertag(conn, kv.first.c_str(), &err);
            }

            printf("[write] Re-registered %zu tags.\n", tagMap.size());
            return conn;
        }
        for (int i = 0; i < config->reconnect_interval / 100 && g_running; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return -1;
}

// ---- 字节序转换 ----

static uint16_t swap16(uint16_t val) {
    return __builtin_bswap16(val);
}

static uint32_t swap32(uint32_t val) {
    return __builtin_bswap32(val);
}

// ---- 写PLC线程 ----

void threadWritePlc(AppConfig* config) {
    printf("[write] Write thread started.\n");

    // 1. 连接gPlat
    int conn = connectgplat(config->gplat_server.c_str(), config->gplat_port);
    if (conn <= 0) {
        printf("[write] Cannot connect to gPlat, retrying...\n");
        while (g_running && conn <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(config->reconnect_interval));
            conn = connectgplat(config->gplat_server.c_str(), config->gplat_port);
        }
        if (conn <= 0) {
            printf("[write] Write thread exiting: cannot connect to gPlat.\n");
            return;
        }
    }
    printf("[write] Connected to gPlat (fd=%d)\n", conn);

    unsigned int err;
    // 订阅timer用于退出检测
    subscribe(conn, "timer_500ms", &err);

    // 2. 为每个PLC创建独立的snap7客户端
    std::map<std::string, S7Object> plcClients; // plc.name -> S7Object
    for (auto& plc : config->plcs) {
        S7Object client = Cli_Create();
        int res = Cli_ConnectTo(client, plc.ip.c_str(), plc.rack, plc.slot);
        if (res != 0) {
            printf("[write thread] snap7 connect to %s (%s) failed (err=%d), will retry on write.\n",
                   plc.name.c_str(), plc.ip.c_str(), res);
        } else {
            printf("[write thread] snap7 connected to %s (%s)\n", plc.name.c_str(), plc.ip.c_str());
        }
        plcClients[plc.name] = client;
    }

    // 3. 构建tag查找映射
    std::map<std::string, TagLookup> tagMap;
    for (auto& plc : config->plcs) {
        for (auto& tag : plc.tags) {
            TagLookup lookup;
            lookup.tag = &tag;
            lookup.plc = &plc;
            lookup.client = plcClients[plc.name];
            tagMap[tag.tagname] = lookup;
        }
    }

    // 4. 注册所有tag
    for (auto& kv : tagMap) {
        registertag(conn, kv.first.c_str(), &err);
    }
    printf("[write] Registered %zu tags.\n", tagMap.size());

    // 5. 主循环
    while (g_running) {
        char value[1024] = {0};
        std::string tagname;

        bool ret = waitpostdata(conn, tagname, value, 1024, -1, &err);

        if (!ret) {
            printf("[write] waitpostdata failed, reconnecting gPlat...\n");
            conn = reconnectGplatWrite(config, tagMap);
            if (conn <= 0) {
                printf("[write] Write thread exiting: gPlat reconnect failed.\n");
                break;
            }
            continue;
        }

        // timer唤醒，仅用于检查g_running
        if (tagname == "timer_500ms") {
            continue;
        }

        if (tagname == "WAIT_TIMEOUT") {
            continue;
        }

        // 查找tag
        auto it = tagMap.find(tagname);
        if (it == tagMap.end()) {
            printf("[write] Unknown tag: '%s', skipping.\n", tagname.c_str());
            continue;
        }

        TagLookup& lookup = it->second;
        TagConfig* tag = lookup.tag;
        S7Object client = lookup.client;
        int area = AreaToSnap7(tag->area);

        // 检查snap7连接
        int connected = 0;
        Cli_GetConnected(client, &connected);
        if (!connected) {
            if (!reconnectSnap7Write(client, *lookup.plc, config->reconnect_interval)) {
                printf("[write] snap7 reconnect failed for %s, skipping write.\n",
                       lookup.plc->name.c_str());
                continue;
            }
        }

        int res = 0;

        switch (tag->datatype) {
            case S7DataType::BOOL: {
                // value中是bool值
                bool bval;
                memcpy(&bval, value, sizeof(bool));

                // 读-改-写
                uint8_t currentByte;
                res = Cli_ReadArea(client, area, tag->dbnumber,
                                   tag->byte_offset, 1, S7WLByte, &currentByte);
                if (res != 0) {
                    printf("[write] BOOL read-modify-write: ReadArea failed (err=%d)\n", res);
                    break;
                }
                if (bval)
                    currentByte |= (1 << tag->bit_offset);
                else
                    currentByte &= ~(1 << tag->bit_offset);

                res = Cli_WriteArea(client, area, tag->dbnumber,
                                    tag->byte_offset, 1, S7WLByte, &currentByte);
                break;
            }
            case S7DataType::INT: {
                short hostVal;
                memcpy(&hostVal, value, sizeof(short));
                uint16_t raw16;
                memcpy(&raw16, &hostVal, 2);
                raw16 = swap16(raw16);
                res = Cli_WriteArea(client, area, tag->dbnumber,
                                    tag->byte_offset, 2, S7WLByte, &raw16);
                break;
            }
            case S7DataType::WORD: {
                uint16_t hostVal;
                memcpy(&hostVal, value, sizeof(uint16_t));
                uint16_t raw16 = swap16(hostVal);
                res = Cli_WriteArea(client, area, tag->dbnumber,
                                    tag->byte_offset, 2, S7WLByte, &raw16);
                break;
            }
            case S7DataType::DINT: {
                int hostVal;
                memcpy(&hostVal, value, sizeof(int));
                uint32_t raw32;
                memcpy(&raw32, &hostVal, 4);
                raw32 = swap32(raw32);
                res = Cli_WriteArea(client, area, tag->dbnumber,
                                    tag->byte_offset, 4, S7WLByte, &raw32);
                break;
            }
            case S7DataType::DWORD: {
                uint32_t hostVal;
                memcpy(&hostVal, value, sizeof(uint32_t));
                uint32_t raw32 = swap32(hostVal);
                res = Cli_WriteArea(client, area, tag->dbnumber,
                                    tag->byte_offset, 4, S7WLByte, &raw32);
                break;
            }
            case S7DataType::REAL: {
                float hostVal;
                memcpy(&hostVal, value, sizeof(float));
                uint32_t raw32;
                memcpy(&raw32, &hostVal, 4);
                raw32 = swap32(raw32);
                res = Cli_WriteArea(client, area, tag->dbnumber,
                                    tag->byte_offset, 4, S7WLByte, &raw32);
                break;
            }
            case S7DataType::STRING: {
                // value中是C字符串
                const char* str = value;
                int actual_len = strlen(str);
                if (actual_len > tag->maxlen) actual_len = tag->maxlen;

                int s7size = tag->maxlen + 2;
                uint8_t s7buf[258] = {0};
                s7buf[0] = (uint8_t)tag->maxlen;
                s7buf[1] = (uint8_t)actual_len;
                memcpy(s7buf + 2, str, actual_len);

                res = Cli_WriteArea(client, area, tag->dbnumber,
                                    tag->byte_offset, s7size, S7WLByte, s7buf);
                break;
            }
        }

        if (res != 0) {
            printf("[write] Cli_WriteArea failed for tag '%s' (err=%d)\n",
                   tag->tagname.c_str(), res);

            // 检查是否断开
            connected = 0;
            Cli_GetConnected(client, &connected);
            if (!connected) {
                reconnectSnap7Write(client, *lookup.plc, config->reconnect_interval);
            }
        }
    }

    // 清理snap7连接
    for (auto& kv : plcClients) {
        Cli_Disconnect(kv.second);
        Cli_Destroy(&kv.second);
    }

    printf("[write] Write thread exited.\n");
}
