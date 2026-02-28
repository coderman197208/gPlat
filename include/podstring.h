#pragma once

// PodString<N> - 栈分配、memcpy安全的固定容量字符串类
//
// 特性:
//   - 完全栈上分配，无堆内存
//   - standard_layout + trivially_copyable，可安全 memcpy
//   - memset(&ps, 0, sizeof(ps)) 产生合法空串
//   - m_data 在 offset 0，对象地址即字符串地址
//   - N 为最大字符数（不含 '\0'），内部缓冲区大小为 N+1
//   - 溢出时抛出 std::length_error

#include <cstddef>
#include <cstring>
#include <string>
#include <stdexcept>
#include <ostream>
#include <type_traits>

template<size_t N>
class PodString
{
    static_assert(N >= 1, "PodString capacity must be at least 1");

private:
    char   m_data[N + 1];  // null-terminated buffer, offset 0
    size_t m_size;       // 当前长度（不含 '\0'）

public:
    // ==================== 构造函数 ====================

    // 默认构造：空串
    PodString() noexcept
        : m_data{'\0'}, m_size{0}
    {
    }

    // 从 C 字符串构造，nullptr 视为空串
    PodString(const char* s)
    {
        if (s == nullptr) {
            m_data[0] = '\0';
            m_size = 0;
            return;
        }
        m_size = std::strlen(s);
        if (m_size > N) {
            throw std::length_error(
                "PodString: input length " + std::to_string(m_size) +
                " exceeds capacity " + std::to_string(N));
        }
        std::memcpy(m_data, s, m_size + 1);
    }

    // 从 std::string 构造
    PodString(const std::string& s)
    {
        m_size = s.size();
        if (m_size > N) {
            throw std::length_error(
                "PodString: input length " + std::to_string(m_size) +
                " exceeds capacity " + std::to_string(N));
        }
        std::memcpy(m_data, s.c_str(), m_size + 1);
    }

    // 默认 复制/移动 构造（保持 trivially_copyable）
    PodString(const PodString&)            = default;
    PodString(PodString&&)                 = default;

    // 默认析构
    ~PodString()                           = default;

    // ==================== 赋值 ====================

    // 默认 复制/移动 赋值
    PodString& operator=(const PodString&) = default;
    PodString& operator=(PodString&&)      = default;

    // 从 C 字符串赋值（memmove：s 可能指向 m_data 内部）
    PodString& operator=(const char* s)
    {
        if (s == nullptr) {
            m_data[0] = '\0';
            m_size = 0;
            return *this;
        }
        size_t len = std::strlen(s);
        if (len > N) {
            throw std::length_error(
                "PodString: input length " + std::to_string(len) +
                " exceeds capacity " + std::to_string(N));
        }
        std::memmove(m_data, s, len + 1);
        m_size = len;
        return *this;
    }

    // 从 std::string 赋值
    PodString& operator=(const std::string& s)
    {
        m_size = s.size();
        if (m_size > N) {
            throw std::length_error(
                "PodString: input length " + std::to_string(m_size) +
                " exceeds capacity " + std::to_string(N));
        }
        std::memcpy(m_data, s.c_str(), m_size + 1);
        return *this;
    }

    // ==================== 访问器 ====================

    const char* c_str()  const noexcept { return m_data; }
    const char* data()   const noexcept { return m_data; }
    size_t      size()   const noexcept { return m_size; }
    size_t      length() const noexcept { return m_size; }
    bool        empty()  const noexcept { return m_size == 0; }

    static constexpr size_t capacity() noexcept { return N; }

    // ==================== 修改器 ====================

    void clear() noexcept
    {
        m_data[0] = '\0';
        m_size = 0;
    }

    // ==================== 元素访问 ====================

    // 无边界检查（匹配 std::string 语义）
    char&       operator[](size_t pos)       noexcept { return m_data[pos]; }
    const char& operator[](size_t pos) const noexcept { return m_data[pos]; }

    // 有边界检查
    char& at(size_t pos)
    {
        if (pos >= m_size) {
            throw std::out_of_range(
                "PodString::at: pos " + std::to_string(pos) +
                " >= size " + std::to_string(m_size));
        }
        return m_data[pos];
    }

    const char& at(size_t pos) const
    {
        if (pos >= m_size) {
            throw std::out_of_range(
                "PodString::at: pos " + std::to_string(pos) +
                " >= size " + std::to_string(m_size));
        }
        return m_data[pos];
    }

    // ==================== 拼接 ====================

    PodString& operator+=(const PodString& other)
    {
        size_t new_size = m_size + other.m_size;
        if (new_size > N) {
            throw std::length_error("PodString: concatenation exceeds capacity");
        }
        // memmove: 自连接安全 (ps += ps)
        std::memmove(m_data + m_size, other.m_data, other.m_size + 1);
        m_size = new_size;
        return *this;
    }

    PodString& operator+=(const char* s)
    {
        if (s == nullptr) return *this;
        size_t len = std::strlen(s);
        if (m_size + len > N) {
            throw std::length_error("PodString: concatenation exceeds capacity");
        }
        // memmove: s 可能指向 m_data (ps += ps.c_str())
        std::memmove(m_data + m_size, s, len + 1);
        m_size += len;
        return *this;
    }

    PodString& operator+=(const std::string& s)
    {
        if (m_size + s.size() > N) {
            throw std::length_error("PodString: concatenation exceeds capacity");
        }
        std::memcpy(m_data + m_size, s.c_str(), s.size() + 1);
        m_size += s.size();
        return *this;
    }

    PodString& operator+=(char c)
    {
        if (m_size + 1 > N) {
            throw std::length_error("PodString: concatenation exceeds capacity");
        }
        m_data[m_size] = c;
        ++m_size;
        m_data[m_size] = '\0';
        return *this;
    }

    // operator+ (friend 内联定义)

    friend PodString operator+(const PodString& lhs, const PodString& rhs)
    {
        PodString result(lhs);
        result += rhs;
        return result;
    }

    friend PodString operator+(const PodString& lhs, const char* rhs)
    {
        PodString result(lhs);
        result += rhs;
        return result;
    }

    friend PodString operator+(const char* lhs, const PodString& rhs)
    {
        PodString result(lhs);
        result += rhs;
        return result;
    }

    friend PodString operator+(const PodString& lhs, const std::string& rhs)
    {
        PodString result(lhs);
        result += rhs;
        return result;
    }

    // ==================== 转换 ====================

    explicit operator std::string() const
    {
        return std::string(m_data, m_size);
    }

    std::string to_string() const
    {
        return std::string(m_data, m_size);
    }

    // ==================== 比较：同类型 PodString ====================

    friend bool operator==(const PodString& a, const PodString& b) noexcept
    {
        return a.m_size == b.m_size &&
               std::memcmp(a.m_data, b.m_data, a.m_size) == 0;
    }

    friend bool operator!=(const PodString& a, const PodString& b) noexcept
    {
        return !(a == b);
    }

    friend bool operator<(const PodString& a, const PodString& b) noexcept
    {
        return std::strcmp(a.m_data, b.m_data) < 0;
    }

    friend bool operator>(const PodString& a, const PodString& b) noexcept
    {
        return b < a;
    }

    friend bool operator<=(const PodString& a, const PodString& b) noexcept
    {
        return !(b < a);
    }

    friend bool operator>=(const PodString& a, const PodString& b) noexcept
    {
        return !(a < b);
    }

    // ==================== 比较：与 const char* ====================

    friend bool operator==(const PodString& a, const char* b) noexcept
    {
        return b != nullptr && std::strcmp(a.m_data, b) == 0;
    }

    friend bool operator==(const char* a, const PodString& b) noexcept
    {
        return b == a;
    }

    friend bool operator!=(const PodString& a, const char* b) noexcept
    {
        return !(a == b);
    }

    friend bool operator!=(const char* a, const PodString& b) noexcept
    {
        return !(b == a);
    }

    // ==================== 比较：与 std::string ====================

    friend bool operator==(const PodString& a, const std::string& b) noexcept
    {
        return a.m_size == b.size() &&
               std::memcmp(a.m_data, b.c_str(), a.m_size) == 0;
    }

    friend bool operator==(const std::string& a, const PodString& b) noexcept
    {
        return b == a;
    }

    friend bool operator!=(const PodString& a, const std::string& b) noexcept
    {
        return !(a == b);
    }

    friend bool operator!=(const std::string& a, const PodString& b) noexcept
    {
        return !(b == a);
    }

    // ==================== 流输出 ====================

    friend std::ostream& operator<<(std::ostream& os, const PodString& s)
    {
        return os.write(s.m_data, s.m_size);
    }
};

// ==================== 编译期布局验证 ====================

static_assert(std::is_standard_layout_v<PodString<64>>,
    "PodString must be standard_layout");
static_assert(std::is_trivially_copyable_v<PodString<64>>,
    "PodString must be trivially_copyable (memcpy-safe)");
static_assert(std::is_trivially_destructible_v<PodString<64>>,
    "PodString must be trivially_destructible");

static_assert(std::is_standard_layout_v<PodString<16>>,
    "PodString<16> must be standard_layout");
static_assert(std::is_trivially_copyable_v<PodString<16>>,
    "PodString<16> must be trivially_copyable");

// ==================== 常用类型别名 ====================

using PodString8   = PodString<8>;
using PodString10 = PodString<10>;
using PodString16  = PodString<16>;
using PodString20  = PodString<20>;
using PodString40  = PodString<40>;
using PodString64  = PodString<64>;
using PodString128 = PodString<128>;
