#pragma once
// 轻量零依赖 UTF-8 安全 JSON 构造器与格式化辅助。
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <string_view>

namespace MiniCAD::SaTool
{
    inline std::string JsonQuote(std::string_view s)
    {
        std::string out;
        out.reserve(s.size() + 8);
        out.push_back('"');
        for (unsigned char c : s)
        {
            switch (c)
            {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20u)
                {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                }
                else
                {
                    out.push_back(static_cast<char>(c));
                }
                break;
            }
        }
        out.push_back('"');
        return out;
    }

    inline std::string JsonDouble(double v, int precision = 4)
    {
        if (!std::isfinite(v))
            return "null";
        if (std::abs(v) < 1e-12)
            v = 0.0;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.*f", precision, v);
        std::string s(buf);
        if (s.find('.') != std::string::npos)
        {
            while (!s.empty() && s.back() == '0') s.pop_back();
            if (!s.empty() && s.back() == '.') s.pop_back();
        }
        if (s == "-0" || s.empty())
            return "0";
        return s;
    }

    inline std::string FormatHandleHex(uint64_t handle)
    {
        if (handle == 0)
            return {};
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%llX", static_cast<unsigned long long>(handle));
        return std::string(buf);
    }

    inline std::string FormatColorRgbHex(double r, double g, double b)
    {
        auto toByte = [](double v) -> unsigned {
            if (v <= 0.0) return 0u;
            if (v >= 1.0) return 255u;
            return static_cast<unsigned>(v * 255.0 + 0.5);
        };
        char buf[16];
        std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", toByte(r), toByte(g), toByte(b));
        return std::string(buf);
    }

    // 简单流式 JSON 构造器（支持紧凑与缩进美化两种输出）
    class JsonWriter
    {
    public:
        explicit JsonWriter(bool pretty = false) : m_pretty(pretty) {}

        const std::string& Str() const { return m_out; }
        std::string Take() { return std::move(m_out); }

        void BeginObject()
        {
            BeforeValue();
            m_out.push_back('{');
            m_stack.push_back({ true, false });
        }

        void EndObject()
        {
            if (m_stack.empty()) return;
            const bool hadItem = m_stack.back().hasItem;
            m_stack.pop_back();
            if (m_pretty && hadItem)
            {
                m_out.push_back('\n');
                Indent();
            }
            m_out.push_back('}');
        }

        void BeginArray()
        {
            BeforeValue();
            m_out.push_back('[');
            m_stack.push_back({ false, false });
        }

        void EndArray()
        {
            if (m_stack.empty()) return;
            const bool hadItem = m_stack.back().hasItem;
            m_stack.pop_back();
            if (m_pretty && hadItem)
            {
                m_out.push_back('\n');
                Indent();
            }
            m_out.push_back(']');
        }

        void Key(std::string_view key)
        {
            if (!m_stack.empty())
            {
                if (m_stack.back().hasItem)
                    m_out.push_back(',');
                if (m_pretty)
                {
                    m_out.push_back('\n');
                    Indent();
                }
                m_stack.back().hasItem = true;
            }
            m_out += JsonQuote(key);
            m_out += m_pretty ? ": " : ":";
            m_expectingValue = true;
        }

        void Null()                     { BeforeValue(); m_out += "null"; }
        void Bool(bool v)               { BeforeValue(); m_out += v ? "true" : "false"; }
        void Int(int64_t v)             { BeforeValue(); m_out += std::to_string(v); }
        void UInt(uint64_t v)           { BeforeValue(); m_out += std::to_string(v); }
        void Double(double v, int p = 4){ BeforeValue(); m_out += JsonDouble(v, p); }
        void String(std::string_view s) { BeforeValue(); m_out += JsonQuote(s); }
        void Raw(std::string_view raw)  { BeforeValue(); m_out += raw; }

        void KeyNull(std::string_view k)                        { Key(k); Null(); }
        void KeyBool(std::string_view k, bool v)                { Key(k); Bool(v); }
        void KeyInt(std::string_view k, int64_t v)              { Key(k); Int(v); }
        void KeyUInt(std::string_view k, uint64_t v)            { Key(k); UInt(v); }
        void KeyDouble(std::string_view k, double v, int p = 4) { Key(k); Double(v, p); }
        void KeyString(std::string_view k, std::string_view s)  { Key(k); String(s); }
        void KeyOptString(std::string_view k, std::string_view s)
        {
            Key(k);
            if (s.empty()) Null();
            else String(s);
        }

    private:
        struct Frame
        {
            bool isObject = false;
            bool hasItem  = false;
        };

        void Indent()
        {
            m_out.append(m_stack.size() * 2u, ' ');
        }

        void BeforeValue()
        {
            if (m_expectingValue)
            {
                m_expectingValue = false;
                return;
            }
            if (!m_stack.empty() && !m_stack.back().isObject)
            {
                if (m_stack.back().hasItem)
                    m_out.push_back(',');
                if (m_pretty)
                {
                    m_out.push_back('\n');
                    Indent();
                }
                m_stack.back().hasItem = true;
            }
        }

        bool               m_pretty = false;
        bool               m_expectingValue = false;
        std::vector<Frame> m_stack;
        std::string        m_out;
    };
}
