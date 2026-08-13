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
#include "s7log.h"

extern std::atomic<bool> g_running;

// tag查找信息
struct TagLookup {
    TagConfig* tag;
    PlcConfig* plc;
    S7Object   client;
};

struct PlcWriteResult {
    int         error;
    const char* operation;
};

static void getSnap7ErrorText(int err, char* buffer, int buffer_size) {
    if (buffer_size <= 0) {
        return;
    }

    if (Cli_ErrorText(err, buffer, buffer_size) != 0) {
        std::snprintf(buffer, buffer_size, "snap7 error %d", err);
    }
    buffer[buffer_size - 1] = '\0';
}

static int getSnap7ConnectedState(S7Object client) {
    int connected = 0;
    if (Cli_GetConnected(client, &connected) != 0) {
        return -1;
    }
    return connected;
}

static bool shouldReconnectAfterIoError(int err) {
    const unsigned int err_class = static_cast<unsigned int>(err) & 0xFFFF0000u;

    switch (err_class) {
        case errIsoConnect:
        case errIsoDisconnect:
        case errIsoInvalidPDU:
        case errIsoShortPacket:
        case errIsoTooManyFragments:
        case errIsoPduOverflow:
        case errIsoSendPacket:
        case errIsoRecvPacket:
        case errCliInvalidPlcAnswer:
        case errCliInvalidDataSizeRecvd:
            return true;
        default:
            return false;
    }
}

static void logSnap7PduWrite(const PlcConfig& plc, S7Object client, const char* stage) {
    int requested = 0;
    int negotiated = 0;
    int res = Cli_GetPduLength(client, &requested, &negotiated);
    if (res == 0) {
        s7log_info("[%s/write] snap7 PDU after %s: requested=%d negotiated=%d",
               plc.name.c_str(), stage, requested, negotiated);
        return;
    }

    char err_text[256] = {0};
    getSnap7ErrorText(res, err_text, sizeof(err_text));
    s7log_warn("[%s/write] Cli_GetPduLength failed after %s: err=%d (0x%08X, %s)",
           plc.name.c_str(), stage, res, static_cast<unsigned int>(res), err_text);
}

static void logPlcWriteError(const PlcConfig& plc, const TagConfig& tag,
                             S7Object client, const PlcWriteResult& result,
                             const char* stage) {
    char err_text[256] = {0};
    getSnap7ErrorText(result.error, err_text, sizeof(err_text));
    const int connected = getSnap7ConnectedState(client);

    s7log_error("[write] %s failed during %s: tag='%s' plc=%s ip=%s area=%s db=%d byte=%d bit=%d err=%d (0x%08X, %s) Cli_GetConnected=%d",
           result.operation, stage, tag.tagname.c_str(), plc.name.c_str(), plc.ip.c_str(),
           AreaName(tag.area), tag.dbnumber, tag.byte_offset, tag.bit_offset,
           result.error, static_cast<unsigned int>(result.error), err_text, connected);
}

// ---- snap7 重连 (写线程用) ----

static bool reconnectSnap7Write(S7Object client, const PlcConfig& plc, int interval) {
    Cli_Disconnect(client);
    unsigned int attempt = 0;
    while (g_running) {
        ++attempt;
        s7log_warn("[%s/write] snap7 reconnect attempt=%u ip=%s rack=%d slot=%d",
               plc.name.c_str(), attempt, plc.ip.c_str(), plc.rack, plc.slot);
        int res = Cli_ConnectTo(client, plc.ip.c_str(), plc.rack, plc.slot);
        if (res == 0) {
            s7log_info("[%s/write] snap7 reconnected: ip=%s attempts=%u",
                   plc.name.c_str(), plc.ip.c_str(), attempt);
            logSnap7PduWrite(plc, client, "reconnect");
            return true;
        }

        char err_text[256] = {0};
        getSnap7ErrorText(res, err_text, sizeof(err_text));
        s7log_warn("[%s/write] snap7 reconnect failed: attempt=%u err=%d (0x%08X, %s), retry_in=%d ms",
               plc.name.c_str(), attempt, res, static_cast<unsigned int>(res),
               err_text, interval);
        for (int i = 0; i < interval / 100 && g_running; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

// ---- gPlat 重连 + 重新注册 ----

static bool registerGplatWriteSubscriptions(int conn, const std::map<std::string, TagLookup>& tagMap) {
    unsigned int err = 0;
    if (!subscribe(conn, "timer_500ms", &err)) {
        s7log_warn("[write] Failed to subscribe timer_500ms, error=%u", err);
        return false;
    }

    size_t registered = 0;
    for (const auto& kv : tagMap) {
        if (!registertag(conn, kv.first.c_str(), &err)) {
            s7log_warn("[write] Failed to register tag '%s', error=%u, registered=%zu/%zu",
                   kv.first.c_str(), err, registered, tagMap.size());
            return false;
        }
        ++registered;
    }
    return true;
}

static int reconnectGplatWrite(AppConfig* config, const std::map<std::string, TagLookup>& tagMap) {
    while (g_running) {
        s7log_warn("[write] gPlat reconnecting to %s:%d...",
               config->gplat_server.c_str(), config->gplat_port);
        int conn = connectgplat(config->gplat_server.c_str(), config->gplat_port);
        if (conn > 0) {
            s7log_info("[write] gPlat reconnected (fd=%d), re-registering...", conn);
            if (registerGplatWriteSubscriptions(conn, tagMap)) {
                s7log_info("[write] Re-registered %zu tags.", tagMap.size());
                return conn;
            }
            s7log_warn("[write] gPlat subscription registration failed, retrying...");
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

static PlcWriteResult writeTagToPlc(S7Object client, const TagConfig& tag, const char* value) {
    const int area = AreaToSnap7(tag.area);

    switch (tag.datatype) {
        case S7DataType::BOOL: {
            bool bool_value;
            memcpy(&bool_value, value, sizeof(bool));

            uint8_t current_byte = 0;
            int res = Cli_ReadArea(client, area, tag.dbnumber,
                                   tag.byte_offset, 1, S7WLByte, &current_byte);
            if (res != 0) {
                return {res, "Cli_ReadArea (BOOL read-modify-write)"};
            }

            if (bool_value) {
                current_byte |= (1 << tag.bit_offset);
            }
            else {
                current_byte &= ~(1 << tag.bit_offset);
            }

            res = Cli_WriteArea(client, area, tag.dbnumber,
                                tag.byte_offset, 1, S7WLByte, &current_byte);
            return {res, "Cli_WriteArea (BOOL read-modify-write)"};
        }
        case S7DataType::INT: {
            short host_value;
            memcpy(&host_value, value, sizeof(short));
            uint16_t raw16;
            memcpy(&raw16, &host_value, sizeof(raw16));
            raw16 = swap16(raw16);
            int res = Cli_WriteArea(client, area, tag.dbnumber,
                                    tag.byte_offset, 2, S7WLByte, &raw16);
            return {res, "Cli_WriteArea"};
        }
        case S7DataType::WORD: {
            uint16_t host_value;
            memcpy(&host_value, value, sizeof(uint16_t));
            uint16_t raw16 = swap16(host_value);
            int res = Cli_WriteArea(client, area, tag.dbnumber,
                                    tag.byte_offset, 2, S7WLByte, &raw16);
            return {res, "Cli_WriteArea"};
        }
        case S7DataType::DINT: {
            int host_value;
            memcpy(&host_value, value, sizeof(int));
            uint32_t raw32;
            memcpy(&raw32, &host_value, sizeof(raw32));
            raw32 = swap32(raw32);
            int res = Cli_WriteArea(client, area, tag.dbnumber,
                                    tag.byte_offset, 4, S7WLByte, &raw32);
            return {res, "Cli_WriteArea"};
        }
        case S7DataType::DWORD: {
            uint32_t host_value;
            memcpy(&host_value, value, sizeof(uint32_t));
            uint32_t raw32 = swap32(host_value);
            int res = Cli_WriteArea(client, area, tag.dbnumber,
                                    tag.byte_offset, 4, S7WLByte, &raw32);
            return {res, "Cli_WriteArea"};
        }
        case S7DataType::REAL: {
            float host_value;
            memcpy(&host_value, value, sizeof(float));
            uint32_t raw32;
            memcpy(&raw32, &host_value, sizeof(raw32));
            raw32 = swap32(raw32);
            int res = Cli_WriteArea(client, area, tag.dbnumber,
                                    tag.byte_offset, 4, S7WLByte, &raw32);
            return {res, "Cli_WriteArea"};
        }
        case S7DataType::STRING: {
            int actual_len = strlen(value);
            if (actual_len > tag.maxlen) {
                actual_len = tag.maxlen;
            }

            int s7size = tag.maxlen + 2;
            uint8_t s7buf[258] = {0};
            s7buf[0] = static_cast<uint8_t>(tag.maxlen);
            s7buf[1] = static_cast<uint8_t>(actual_len);
            memcpy(s7buf + 2, value, actual_len);

            int res = Cli_WriteArea(client, area, tag.dbnumber,
                                    tag.byte_offset, s7size, S7WLByte, s7buf);
            return {res, "Cli_WriteArea"};
        }
    }

    return {errCliInvalidParams, "unsupported datatype"};
}

// ---- 写PLC线程 ----

void threadWritePlc(AppConfig* config) {
    s7log_info("[write] Write thread started.");

    // 1. 连接gPlat
    int conn = connectgplat(config->gplat_server.c_str(), config->gplat_port);
    if (conn <= 0) {
        s7log_warn("[write] Cannot connect to gPlat, retrying...");
        while (g_running && conn <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(config->reconnect_interval));
            conn = connectgplat(config->gplat_server.c_str(), config->gplat_port);
        }
        if (conn <= 0) {
            s7log_error("[write] Write thread exiting: cannot connect to gPlat.");
            return;
        }
    }
    s7log_info("[write] Connected to gPlat (fd=%d)", conn);

    // 2. 为每个PLC创建独立的snap7客户端
    std::map<std::string, S7Object> plcClients; // plc.name -> S7Object
    if (config->enable_plc_write) {
        for (auto& plc : config->plcs) {
            S7Object client = Cli_Create();
            int res = Cli_ConnectTo(client, plc.ip.c_str(), plc.rack, plc.slot);
            if (res != 0) {
                char err_text[256] = {0};
                getSnap7ErrorText(res, err_text, sizeof(err_text));
                s7log_warn("[write thread] snap7 connect failed: plc=%s ip=%s rack=%d slot=%d err=%d (0x%08X, %s), will retry on write",
                       plc.name.c_str(), plc.ip.c_str(), plc.rack, plc.slot,
                       res, static_cast<unsigned int>(res), err_text);
            } else {
                s7log_info("[write thread] snap7 connected to %s (%s)", plc.name.c_str(), plc.ip.c_str());
                logSnap7PduWrite(plc, client, "connect");
            }
            plcClients[plc.name] = client;
        }
    } else {
        s7log_warn("[write] PLC write disabled by config. Incoming updates will not be written to PLC.");
    }

    // 3. 构建tag查找映射
    std::map<std::string, TagLookup> tagMap;
    for (auto& plc : config->plcs) {
        for (auto& tag : plc.tags) {
            TagLookup lookup;
            lookup.tag = &tag;
            lookup.plc = &plc;
            lookup.client = config->enable_plc_write ? plcClients[plc.name] : 0;
            tagMap[tag.tagname] = lookup;
        }
    }

    // 4. 订阅timer并注册所有tag
    if (!registerGplatWriteSubscriptions(conn, tagMap)) {
        s7log_warn("[write] Initial gPlat subscription registration failed, reconnecting...");
        conn = reconnectGplatWrite(config, tagMap);
    }
    else {
        s7log_info("[write] Registered %zu tags.", tagMap.size());
    }

    // 5. 主循环
    unsigned int err = 0;
    while (g_running && conn > 0) {
        char value[1024] = {0};
        std::string tagname;

        bool ret = waitpostdata(conn, tagname, value, 1024, -1, &err);

        if (!ret) {
            s7log_warn("[write] waitpostdata failed, error = %u, reconnecting gPlat...", err);
            conn = -1;
            conn = reconnectGplatWrite(config, tagMap);
            if (conn <= 0) {
                s7log_error("[write] Write thread exiting: gPlat reconnect failed.");
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
            s7log_warn("[write] Unknown tag: '%s', skipping.", tagname.c_str());
            continue;
        }

        TagLookup& lookup = it->second;
        TagConfig* tag = lookup.tag;

        if (!config->enable_plc_write) {
            s7log_debug("[write] PLC write disabled, skipped tag '%s'.", tag->tagname.c_str());
            continue;
        }

        S7Object client = lookup.client;

        // 检查snap7连接
        int connected = getSnap7ConnectedState(client);
        if (connected != 1) {
            s7log_warn("[write] snap7 is not connected before write: tag='%s' plc=%s ip=%s Cli_GetConnected=%d",
                   tag->tagname.c_str(), lookup.plc->name.c_str(), lookup.plc->ip.c_str(), connected);
            if (!reconnectSnap7Write(client, *lookup.plc, config->reconnect_interval)) {
                s7log_warn("[write] snap7 reconnect failed for %s, skipping write.",
                       lookup.plc->name.c_str());
                continue;
            }
        }

        PlcWriteResult write_result = writeTagToPlc(client, *tag, value);
        if (write_result.error == 0) {
            continue;
        }

        logPlcWriteError(*lookup.plc, *tag, client, write_result, "initial attempt");
        connected = getSnap7ConnectedState(client);
        if (!shouldReconnectAfterIoError(write_result.error) && connected == 1) {
            s7log_warn("[write] PLC operation error is not a transport/session error; write event abandoned without reconnect: tag='%s' plc=%s err=%d",
                   tag->tagname.c_str(), lookup.plc->name.c_str(), write_result.error);
            continue;
        }

        s7log_warn("[write] snap7 session is unreliable after %s failure; reconnecting before one retry: tag='%s' plc=%s Cli_GetConnected=%d",
               write_result.operation, tag->tagname.c_str(), lookup.plc->name.c_str(), connected);
        if (!reconnectSnap7Write(client, *lookup.plc, config->reconnect_interval)) {
            if (g_running) {
                s7log_error("[write] snap7 reconnect failed; write event abandoned: tag='%s' plc=%s",
                       tag->tagname.c_str(), lookup.plc->name.c_str());
            }
            continue;
        }

        s7log_info("[write] retrying PLC write after reconnect: tag='%s' plc=%s",
               tag->tagname.c_str(), lookup.plc->name.c_str());
        write_result = writeTagToPlc(client, *tag, value);
        if (write_result.error == 0) {
            s7log_info("[write] PLC write recovered after reconnect: tag='%s' plc=%s",
                   tag->tagname.c_str(), lookup.plc->name.c_str());
            continue;
        }

        logPlcWriteError(*lookup.plc, *tag, client, write_result, "retry after reconnect");
        s7log_error("[write] PLC write event abandoned after one retry: tag='%s' plc=%s err=%d",
               tag->tagname.c_str(), lookup.plc->name.c_str(), write_result.error);

        connected = getSnap7ConnectedState(client);
        if (shouldReconnectAfterIoError(write_result.error) || connected != 1) {
            s7log_warn("[write] retry left snap7 session unreliable; reconnecting for subsequent events: plc=%s Cli_GetConnected=%d",
                   lookup.plc->name.c_str(), connected);
            reconnectSnap7Write(client, *lookup.plc, config->reconnect_interval);
        }
    }

    if (conn > 0) {
        disconnectgplat(conn);
    }

    // 清理snap7连接
    for (auto& kv : plcClients) {
        Cli_Disconnect(kv.second);
        Cli_Destroy(&kv.second);
    }

    s7log_info("[write] Write thread exited.");
}
