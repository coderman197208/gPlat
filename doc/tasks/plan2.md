# PodString<N> 实现计划

## Context

gPlat 项目中所有固定大小字符串均以原始 `char` 数组存储（如 `char qname[40]`、`char ip[16]`），缺乏类型安全和便捷的字符串操作。需要一个栈分配、memcpy 安全的字符串类，用于网络协议/IPC 消息场景。

## 文件

- **新建**: `include/podstring.h` — 头文件唯一实现（模板类必须 header-only）

## 内部布局

```cpp
template<size_t N>
class PodString {
    char   m_data[N];   // offset 0, null-terminated
    size_t m_size;      // 当前长度（不含 '\0'）
};
```

- `m_data` 在 offset 0，`reinterpret_cast<const char*>(&obj) == obj.c_str()`
- 成员私有，通过 `static_assert(is_standard_layout)` 保证布局
- `memset(&ps, 0, sizeof(ps))` 产生合法空字符串
- `static_assert(N >= 2)` 防止无意义的实例化

## 设计决策

| 决策 | 选择 | 理由 |
|------|------|------|
| std::string 转换 | `explicit operator std::string()` + `to_string()` | 避免 IPC 热路径上隐式堆分配 |
| operator[] | 不检查边界 | 匹配 std::string 语义；另提供 `at()` 抛异常 |
| nullptr 处理 | 构造/赋值产生空串，比较返回 false | 安全且一致 |
| 自连接安全 | `ps += ps` 使用 memmove | 源和目标可能重叠 |
| 自赋值安全 | `ps = ps.c_str()` 使用 memmove | 指向自身数据 |

## API 清单

**构造/赋值:**
- `PodString()` — 默认空串
- `PodString(const char*)` — 从 C 串，nullptr→空串，溢出→throw
- `PodString(const std::string&)` — 从 std::string，溢出→throw
- `operator=(const char*)`, `operator=(const std::string&)` — 赋值
- 默认复制/移动构造和赋值 (`= default`)

**访问:**
- `c_str()`, `data()` → `const char*`
- `size()`, `length()` → `size_t`
- `capacity()` → `constexpr N-1`
- `empty()` → `bool`
- `clear()` → `void`
- `operator[]` — 无检查
- `at()` — 抛 `std::out_of_range`

**拼接:**
- `operator+=` — PodString / const char* / std::string / char
- `operator+` — friend，同类型组合

**比较:**
- `==`, `!=`, `<`, `>`, `<=`, `>=` — 同大小 PodString
- `==`, `!=` — 与 `const char*` 和 `std::string`

**转换/输出:**
- `explicit operator std::string()`
- `to_string()` → `std::string`
- `operator<<` — ostream 输出

## static_assert 验证

```cpp
static_assert(std::is_standard_layout_v<PodString<64>>);
static_assert(std::is_trivially_copyable_v<PodString<64>>);
static_assert(std::is_trivially_destructible_v<PodString<64>>);
```

关键：`= default` 的复制/移动/析构保持 trivially_copyable；用户定义的其他构造函数不影响此特性。

## 类型别名

```cpp
using PodString8   = PodString<8>;
using PodString16  = PodString<16>;
using PodString20  = PodString<20>;
using PodString40  = PodString<40>;
using PodString64  = PodString<64>;
using PodString128 = PodString<128>;
```

## 依赖

仅标准库：`<cstring>`, `<string>`, `<stdexcept>`, `<ostream>`, `<type_traits>`

## 验证方式

编译即验证：`static_assert` 在编译期检查布局特性。可在 testapp2 中添加简单测试验证：
- 构造/赋值/溢出异常
- memcpy 往返一致性
- 自连接 (`ps += ps`)
- 用作 `std::map` 键
