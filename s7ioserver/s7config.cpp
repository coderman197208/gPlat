#include "s7config.h"
#include "../include/snap7.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdio>
#include <set>
#include <map>
#include <cstring>

// ---- 辅助函数 ----

// snap7区域常量转换
int AreaToSnap7(S7AreaType area) {
    switch (area) {
        case S7AreaType::DB: return S7AreaDB;
        case S7AreaType::I:  return S7AreaPE;
        case S7AreaType::Q:  return S7AreaPA;
        case S7AreaType::M:  return S7AreaMK;
    }
    return S7AreaDB;
}

// 区域名称（用于日志）
const char* AreaName(S7AreaType area) {
    switch (area) {
        case S7AreaType::DB: return "DB";
        case S7AreaType::I:  return "I";
        case S7AreaType::Q:  return "Q";
        case S7AreaType::M:  return "M";
    }
    return "??";
}

// 去除字符串前后空白
static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// 解析数据类型
static bool parseDataType(const std::string& s, S7DataType& dt) {
    std::string upper = s;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
    if (upper == "BOOL")   { dt = S7DataType::BOOL;   return true; }
    if (upper == "INT")    { dt = S7DataType::INT;    return true; }
    if (upper == "WORD")   { dt = S7DataType::WORD;   return true; }
    if (upper == "DINT")   { dt = S7DataType::DINT;   return true; }
    if (upper == "DWORD")  { dt = S7DataType::DWORD;  return true; }
    if (upper == "REAL")   { dt = S7DataType::REAL;   return true; }
    if (upper == "STRING") { dt = S7DataType::STRING; return true; }
    return false;
}

// 解析区域类型
static bool parseAreaType(const std::string& s, S7AreaType& at) {
    std::string upper = s;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
    if (upper == "DB") { at = S7AreaType::DB; return true; }
    if (upper == "I" || upper == "EB")  { at = S7AreaType::I; return true; }
    if (upper == "Q" || upper == "AB")  { at = S7AreaType::Q; return true; }
    if (upper == "M" || upper == "MK")  { at = S7AreaType::M; return true; }
    return false;
}

// 计算数据类型在PLC中的字节大小
static int calcByteSize(S7DataType dt, int maxlen) {
    switch (dt) {
        case S7DataType::BOOL:   return 1;
        case S7DataType::INT:    return 2;
        case S7DataType::WORD:   return 2;
        case S7DataType::DINT:   return 4;
        case S7DataType::DWORD:  return 4;
        case S7DataType::REAL:   return 4;
        case S7DataType::STRING: return maxlen + 2;
    }
    return 1;
}

// 按逗号分割字符串
static std::vector<std::string> splitComma(const std::string& s) {
    std::vector<std::string> parts;
    std::istringstream iss(s);
    std::string part;
    while (std::getline(iss, part, ',')) {
        parts.push_back(trim(part));
    }
    return parts;
}

// 解析tag定义: area, dbnumber, offset[.bit], datatype[, maxlen]
static bool parseTagValue(const std::string& value, TagConfig& tag) {
    std::vector<std::string> parts = splitComma(value);
    if (parts.size() < 4 || parts.size() > 5) {
        fprintf(stderr, "  Invalid tag format (expected 4-5 fields): %s\n", value.c_str());
        return false;
    }

    // area
    if (!parseAreaType(parts[0], tag.area)) {
        fprintf(stderr, "  Unknown area type: %s\n", parts[0].c_str());
        return false;
    }

    // dbnumber
    tag.dbnumber = std::stoi(parts[1]);

    // offset[.bit]
    std::string offsetStr = parts[2];
    size_t dotPos = offsetStr.find('.');
    if (dotPos != std::string::npos) {
        tag.byte_offset = std::stoi(offsetStr.substr(0, dotPos));
        tag.bit_offset = std::stoi(offsetStr.substr(dotPos + 1));
    } else {
        tag.byte_offset = std::stoi(offsetStr);
        tag.bit_offset = 0;
    }

    // datatype
    if (!parseDataType(parts[3], tag.datatype)) {
        fprintf(stderr, "  Unknown data type: %s\n", parts[3].c_str());
        return false;
    }

    // maxlen (仅STRING)
    tag.maxlen = 254;
    if (parts.size() == 5) {
        tag.maxlen = std::stoi(parts[4]);
    }

    // 计算byte_size
    tag.byte_size = calcByteSize(tag.datatype, tag.maxlen);

    // 初始化变化检测
    tag.first_read = true;

    return true;
}

// ---- BuildReadGroups ----

static const int GAP_THRESHOLD = 32; // 间隔≤32字节的tag合并为一组

static void BuildReadGroups(PlcConfig& plc) {
    // 按(area, dbnumber)分组tag指针
    struct TagKey {
        S7AreaType area;
        int dbnumber;
        bool operator<(const TagKey& o) const {
            if (area != o.area) return area < o.area;
            return dbnumber < o.dbnumber;
        }
    };

    std::map<TagKey, std::vector<TagConfig*>> groups;
    for (auto& tag : plc.tags) {
        TagKey key{tag.area, tag.dbnumber};
        groups[key].push_back(&tag);
    }

    plc.read_groups.clear();

    for (auto& kv : groups) {
        auto& tagPtrs = kv.second;

        // 按byte_offset排序
        std::sort(tagPtrs.begin(), tagPtrs.end(), [](TagConfig* a, TagConfig* b) {
            return a->byte_offset < b->byte_offset;
        });

        // 合并间隔<=GAP_THRESHOLD的tag
        ReadGroup current;
        current.area = kv.first.area;
        current.dbnumber = kv.first.dbnumber;
        current.start_byte = tagPtrs[0]->byte_offset;
        int end_byte = current.start_byte + tagPtrs[0]->byte_size;
        current.tags.push_back(tagPtrs[0]);

        for (size_t i = 1; i < tagPtrs.size(); i++) {
            int tag_start = tagPtrs[i]->byte_offset;
            int tag_end = tag_start + tagPtrs[i]->byte_size;

            if (tag_start - end_byte <= GAP_THRESHOLD) {
                // 合并
                current.tags.push_back(tagPtrs[i]);
                if (tag_end > end_byte) end_byte = tag_end;
            } else {
                // 完成当前组
                current.total_bytes = end_byte - current.start_byte;
                current.buffer.resize(current.total_bytes);
                plc.read_groups.push_back(std::move(current));

                // 开始新组
                current = ReadGroup();
                current.area = kv.first.area;
                current.dbnumber = kv.first.dbnumber;
                current.start_byte = tag_start;
                end_byte = tag_end;
                current.tags.push_back(tagPtrs[i]);
            }
        }

        // 最后一组
        current.total_bytes = end_byte - current.start_byte;
        current.buffer.resize(current.total_bytes);
        plc.read_groups.push_back(std::move(current));
    }
}

// ---- LoadConfig ----

bool LoadConfig(const std::string& filename, AppConfig& config) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        fprintf(stderr, "Cannot open config file: %s\n", filename.c_str());
        return false;
    }

    // 默认值
    config.gplat_server = "127.0.0.1";
    config.gplat_port = 8777;
    config.board_name = "BOARD";
    config.reconnect_interval = 3000;

    std::string line;
    std::string currentSection;
    PlcConfig* currentPlc = nullptr;
    std::set<std::string> allTagNames; // 全局tag名唯一性检查
    int lineNum = 0;

    while (std::getline(file, line)) {
        lineNum++;

        // 去除行内注释（#）
        size_t commentPos = line.find('#');
        if (commentPos != std::string::npos)
            line = line.substr(0, commentPos);

        line = trim(line);

        // 跳过空行
        if (line.empty())
            continue;

        // section头
        if (line[0] == '[') {
            size_t endBracket = line.find(']');
            if (endBracket == std::string::npos) {
                fprintf(stderr, "Line %d: malformed section header: %s\n", lineNum, line.c_str());
                return false;
            }
            currentSection = trim(line.substr(1, endBracket - 1));

            if (currentSection != "general") {
                // 新PLC section
                config.plcs.emplace_back();
                currentPlc = &config.plcs.back();
                currentPlc->name = currentSection;
                currentPlc->rack = 0;
                currentPlc->slot = 1;
                currentPlc->poll_interval = 200;
            } else {
                currentPlc = nullptr;
            }
            continue;
        }

        // key = value
        size_t eqPos = line.find('=');
        if (eqPos == std::string::npos) {
            fprintf(stderr, "Line %d: no '=' found: %s\n", lineNum, line.c_str());
            continue;
        }
        std::string key = trim(line.substr(0, eqPos));
        std::string value = trim(line.substr(eqPos + 1));

        if (currentSection == "general") {
            // 全局参数
            if (key == "gplat_server")       config.gplat_server = value;
            else if (key == "gplat_port")    config.gplat_port = std::stoi(value);
            else if (key == "board_name")    config.board_name = value;
            else if (key == "reconnect_interval") config.reconnect_interval = std::stoi(value);
            else fprintf(stderr, "Line %d: unknown general key: %s\n", lineNum, key.c_str());
        } else if (currentPlc) {
            // PLC参数或tag定义
            if (key == "ip")             currentPlc->ip = value;
            else if (key == "rack")             currentPlc->rack = std::stoi(value);
            else if (key == "slot")             currentPlc->slot = std::stoi(value);
            else if (key == "poll_interval")    currentPlc->poll_interval = std::stoi(value);
            else {
                // 其余键视为tag定义
                std::string tagname = key;

                // 全局唯一性检查
                if (allTagNames.count(tagname)) {
                    fprintf(stderr, "Line %d: duplicate tag name '%s'\n", lineNum, tagname.c_str());
                    return false;
                }

                TagConfig tag;
                tag.tagname = tagname;
                if (!parseTagValue(value, tag)) {
                    fprintf(stderr, "Line %d: failed to parse tag '%s'\n", lineNum, tagname.c_str());
                    return false;
                }

                allTagNames.insert(tagname);
                currentPlc->tags.push_back(std::move(tag));
            }
        }
    }

    // 为每个PLC构建ReadGroups
    for (auto& plc : config.plcs) {
        if (!plc.tags.empty()) {
            BuildReadGroups(plc);
        }
    }

    return true;
}

// ---- PrintConfig ----

static const char* DataTypeName(S7DataType dt) {
    switch (dt) {
        case S7DataType::BOOL:   return "BOOL";
        case S7DataType::INT:    return "INT";
        case S7DataType::WORD:   return "WORD";
        case S7DataType::DINT:   return "DINT";
        case S7DataType::DWORD:  return "DWORD";
        case S7DataType::REAL:   return "REAL";
        case S7DataType::STRING: return "STRING";
    }
    return "??";
}

void PrintConfig(const AppConfig& config) {
    printf("=== s7ioserver Configuration ===\n");
    printf("gPlat server: %s:%d\n", config.gplat_server.c_str(), config.gplat_port);
    printf("Board name: %s\n", config.board_name.c_str());
    printf("Reconnect interval: %d ms\n", config.reconnect_interval);
    printf("PLC count: %zu\n", config.plcs.size());

    for (const auto& plc : config.plcs) {
        printf("\n[%s]\n", plc.name.c_str());
        printf("  IP: %s, Rack: %d, Slot: %d\n", plc.ip.c_str(), plc.rack, plc.slot);
        printf("  Poll interval: %d ms\n", plc.poll_interval);
        printf("  Tags (%zu):\n", plc.tags.size());

        for (const auto& tag : plc.tags) {
            if (tag.datatype == S7DataType::BOOL) {
                printf("    %-20s %s%d byte %d.%d  %s  (%d bytes)\n",
                       tag.tagname.c_str(), AreaName(tag.area), tag.dbnumber,
                       tag.byte_offset, tag.bit_offset,
                       DataTypeName(tag.datatype), tag.byte_size);
            } else if (tag.datatype == S7DataType::STRING) {
                printf("    %-20s %s%d byte %d     %s[%d]  (%d bytes)\n",
                       tag.tagname.c_str(), AreaName(tag.area), tag.dbnumber,
                       tag.byte_offset,
                       DataTypeName(tag.datatype), tag.maxlen, tag.byte_size);
            } else {
                printf("    %-20s %s%d byte %d     %s  (%d bytes)\n",
                       tag.tagname.c_str(), AreaName(tag.area), tag.dbnumber,
                       tag.byte_offset,
                       DataTypeName(tag.datatype), tag.byte_size);
            }
        }

        printf("  Read groups (%zu):\n", plc.read_groups.size());
        for (const auto& group : plc.read_groups) {
            printf("    %s%d [%d..%d] (%d bytes, %zu tags)\n",
                   AreaName(group.area), group.dbnumber,
                   group.start_byte, group.start_byte + group.total_bytes - 1,
                   group.total_bytes, group.tags.size());
        }
    }
    printf("================================\n");
}
