#include <thread>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <cstring>
#include <chrono>
#include <unistd.h>

#include "../include/higplat.h"
#include "../include/snap7.h"
#include "s7config.h"
#include "s7log.h"

extern "C" bool writeb_notpost(int sockfd, const char* tagname, void* value, int actsize, unsigned int* error);
extern "C" bool writeb_string_notpost(int sockfd, const char* tagname, const char* value, unsigned int* error);

extern std::atomic<bool> g_running;

// ---- 字节序转换 ----

static uint16_t swap16(uint16_t val) {
    return __builtin_bswap16(val);
}

static uint32_t swap32(uint32_t val) {
    return __builtin_bswap32(val);
}

// ---- snap7 重连 ----

static bool reconnectSnap7(S7Object client, const PlcConfig& plc, int interval) {
    Cli_Disconnect(client);
    while (g_running) {
        s7log_warn("[%s] snap7 reconnecting to %s...", plc.name.c_str(), plc.ip.c_str());
        int res = Cli_ConnectTo(client, plc.ip.c_str(), plc.rack, plc.slot);
        if (res == 0) {
            s7log_info("[%s] snap7 reconnected.", plc.name.c_str());
            return true;
        }
        for (int i = 0; i < interval / 100 && g_running; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

// ---- gPlat 重连 ----

static int reconnectGplat(const AppConfig& config) {
    while (g_running) {
        s7log_warn("gPlat reconnecting to %s:%d...",
               config.gplat_server.c_str(), config.gplat_port);
        int conn = connectgplat(config.gplat_server.c_str(), config.gplat_port);
        if (conn > 0) {
            s7log_info("gPlat reconnected (fd=%d).", conn);
            return conn;
        }
        for (int i = 0; i < config.reconnect_interval / 100 && g_running; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return -1;
}

// ---- 读PLC线程 ----

void threadReadPlc(PlcConfig* plc, AppConfig* config) {
    s7log_info("[%s] Read thread started for %s", plc->name.c_str(), plc->ip.c_str());

    // 1. 连接gPlat
    int conn = connectgplat(config->gplat_server.c_str(), config->gplat_port);
    if (conn <= 0) {
        conn = reconnectGplat(*config);
        if (conn <= 0) {
            s7log_error("[%s] Read thread exiting: cannot connect to gPlat.", plc->name.c_str());
            return;
        }
    }
    s7log_info("[%s] Connected to gPlat (fd=%d)", plc->name.c_str(), conn);

    // 2. 创建snap7客户端，连接PLC
    S7Object client = Cli_Create();
    int res = Cli_ConnectTo(client, plc->ip.c_str(), plc->rack, plc->slot);
    if (res != 0) {
        s7log_warn("[%s] Initial snap7 connect failed (err=%d), entering reconnect loop.",
               plc->name.c_str(), res);
        if (!reconnectSnap7(client, *plc, config->reconnect_interval)) {
            Cli_Destroy(&client);
            s7log_error("[%s] Read thread exiting.", plc->name.c_str());
            return;
        }
    }
    s7log_info("[%s] Connected to PLC %s", plc->name.c_str(), plc->ip.c_str());

    // 3. 初始化变化检测
    for (auto& tag : plc->tags) {
        tag.last_raw_value.assign(tag.byte_size, 0);
        tag.first_read = true;
    }

    // 4. 主循环
    unsigned int gplat_error;
    while (g_running) {
        bool snap7_ok = true;

        // 对每个ReadGroup读取
        for (auto& group : plc->read_groups) {
            res = Cli_ReadArea(client, AreaToSnap7(group.area), group.dbnumber,
                               group.start_byte, group.total_bytes, S7WLByte,
                               group.buffer.data());
            if (res != 0) {
                s7log_error("[%s] Cli_ReadArea failed: area=%s%d start=%d bytes=%d err=%d",
                       plc->name.c_str(), AreaName(group.area), group.dbnumber,
                       group.start_byte, group.total_bytes, res);

                // 检查是否断开
                int connected = 0;
                Cli_GetConnected(client, &connected);
                if (!connected) {
                    snap7_ok = false;
                    break;
                }
                continue; // 其他错误，跳过此组
            }

            // 处理组内每个tag
            for (auto* tag : group.tags) {
                int buf_offset = tag->byte_offset - group.start_byte;
                uint8_t* raw = group.buffer.data() + buf_offset;

                // 比较是否变化
                bool changed = tag->first_read;
                if (!changed) {
                    if (tag->datatype == S7DataType::BOOL) {
                        bool current_bit = ((raw[0] >> tag->bit_offset) & 0x01) != 0;
                        bool last_bit = ((tag->last_raw_value[0] >> tag->bit_offset) & 0x01) != 0;
                        changed = (current_bit != last_bit);
                    }
                    else {
                        changed = (memcmp(raw, tag->last_raw_value.data(), tag->byte_size) != 0);
                    }
                }

                if (!changed) continue;

                // 更新last_raw_value
                memcpy(tag->last_raw_value.data(), raw, tag->byte_size);   

                // 按类型转换并写入Board
                bool write_ok = false;

                switch (tag->datatype) {
                    case S7DataType::BOOL: {
                        uint8_t byte_val = raw[0];
                        bool bval = (byte_val >> tag->bit_offset) & 0x01;

                        if (tag->first_read) {
                            write_ok = writeb_notpost(conn, tag->tagname.c_str(), &bval, sizeof(bool), &gplat_error);
						}
                        else {
                            write_ok = writeb(conn, tag->tagname.c_str(), &bval, sizeof(bool), &gplat_error);
						}
                        break;
                    }
                    case S7DataType::INT: {
                        uint16_t raw16;
                        memcpy(&raw16, raw, 2);
                        raw16 = swap16(raw16);
                        short val;
                        memcpy(&val, &raw16, 2);

                        if (tag->first_read) {
                            write_ok = writeb_notpost(conn, tag->tagname.c_str(), &val, sizeof(short), &gplat_error);
                        }
                        else {
                            write_ok = writeb(conn, tag->tagname.c_str(), &val, sizeof(short), &gplat_error);
                        }
                        break;
                    }
                    case S7DataType::WORD: {
                        uint16_t raw16;
                        memcpy(&raw16, raw, 2);
                        uint16_t val = swap16(raw16);

                        if (tag->first_read) {
                            write_ok = writeb_notpost(conn, tag->tagname.c_str(), &val, sizeof(uint16_t), &gplat_error);
                        }
                        else {
                            write_ok = writeb(conn, tag->tagname.c_str(), &val, sizeof(uint16_t), &gplat_error);
                        }
                        break;
                    }
                    case S7DataType::DINT: {
                        uint32_t raw32;
                        memcpy(&raw32, raw, 4);
                        raw32 = swap32(raw32);
                        int val;
                        memcpy(&val, &raw32, 4);

                        if (tag->first_read) {
                            write_ok = writeb_notpost(conn, tag->tagname.c_str(), &val, sizeof(int), &gplat_error);
                        }
                        else {
                            write_ok = writeb(conn, tag->tagname.c_str(), &val, sizeof(int), &gplat_error);
                        }
                        break;
                    }
                    case S7DataType::DWORD: {
                        uint32_t raw32;
                        memcpy(&raw32, raw, 4);
                        uint32_t val = swap32(raw32);

                        if (tag->first_read) {
                            write_ok = writeb_notpost(conn, tag->tagname.c_str(), &val, sizeof(uint32_t), &gplat_error);
                        }
                        else {
                            write_ok = writeb(conn, tag->tagname.c_str(), &val, sizeof(uint32_t), &gplat_error);
                        }
                        break;
                    }
                    case S7DataType::REAL: {
                        uint32_t raw32;
                        memcpy(&raw32, raw, 4);
                        raw32 = swap32(raw32);
                        float val;
                        memcpy(&val, &raw32, 4);

                        if (tag->first_read) {
                            write_ok = writeb_notpost(conn, tag->tagname.c_str(), &val, sizeof(float), &gplat_error);
                        }
                        else {
                            write_ok = writeb(conn, tag->tagname.c_str(), &val, sizeof(float), &gplat_error);
                        }
                        break;
                    }
                    case S7DataType::STRING: {
                        // S7 String: byte0=maxlen, byte1=actuallen, byte2+=data
                        uint8_t actual_len = raw[1];
                        if (actual_len > raw[0]) actual_len = raw[0];
                        if (actual_len > tag->maxlen) actual_len = tag->maxlen;

                        char strbuf[258] = {0}; // max 254 chars + null
                        memcpy(strbuf, raw + 2, actual_len);
                        strbuf[actual_len] = '\0';

                        if (tag->first_read) {
                            write_ok = writeb_string_notpost(conn, tag->tagname.c_str(), strbuf, &gplat_error);
                        }
                        else {
                            write_ok = writeb_string(conn, tag->tagname.c_str(), strbuf, &gplat_error);
                        }
                        break;
                    }
                }

                tag->first_read = false;

                if (!write_ok) {
                    s7log_error("[%s] writeb failed for tag '%s', error = %d, reconnecting gPlat...",
                           plc->name.c_str(), tag->tagname.c_str(), gplat_error);
                    conn = reconnectGplat(*config);
                    if (conn <= 0) {
                        s7log_error("[%s] Read thread exiting: gPlat reconnect failed.", plc->name.c_str());
                        Cli_Disconnect(client);
                        Cli_Destroy(&client);
                        return;
                    }
                }
            }
        }

        // snap7断开，重连
        if (!snap7_ok) {
            if (!reconnectSnap7(client, *plc, config->reconnect_interval)) {
                s7log_error("[%s] Read thread exiting: snap7 reconnect failed.", plc->name.c_str());
                break;
            }
            // 重连后标记所有tag需要首次读取
            for (auto& tag : plc->tags) {
                tag.first_read = true;
            }
        }

        // 等待轮询间隔
        for (int i = 0; i < plc->poll_interval / 10 && g_running; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    // 清理
    Cli_Disconnect(client);
    Cli_Destroy(&client);
    s7log_info("[%s] Read thread exited.", plc->name.c_str());
}
