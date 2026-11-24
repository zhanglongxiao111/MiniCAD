#pragma once
// 嵌入模式：导出当前活动文档选中图元的精简摘要 JSON（供宿主面板与 AI 上下文消费）。
// 纯头文件、平台无关，核心库单元测试可直接调用验证。
#include "Scene/Scene.h"
#include "Document/Document.h"
#include "Editor/Editor.h"
#include "Import/CadExchange.h"
#include "Core/MTextFormat.h"
#include "Core/Entity/ArcEntity.hpp"
#include "Core/Entity/AttribEntity.hpp"
#include "Core/Entity/CircleEntity.hpp"
#include "Core/Entity/DimensionEntity.hpp"
#include "Core/Entity/EllipseEntity.hpp"
#include "Core/Entity/Face3DEntity.hpp"
#include "Core/Entity/HatchEntity.hpp"
#include "Core/Entity/ImageEntity.hpp"
#include "Core/Entity/InsertEntity.hpp"
#include "Core/Entity/LeaderEntity.hpp"
#include "Core/Entity/LineEntity.hpp"
#include "Core/Entity/MeshEntity.hpp"
#include "Core/Entity/MLeaderEntity.hpp"
#include "Core/Entity/MLineEntity.hpp"
#include "Core/Entity/MTextEntity.hpp"
#include "Core/Entity/OleFrameEntity.hpp"
#include "Core/Entity/PointEntity.hpp"
#include "Core/Entity/PolylineEntity.hpp"
#include "Core/Entity/RayEntity.hpp"
#include "Core/Entity/RectangleEntity.hpp"
#include "Core/Entity/RegionEntity.hpp"
#include "Core/Entity/ShapeEntity.hpp"
#include "Core/Entity/Solid3DEntity.hpp"
#include "Core/Entity/SolidEntity.hpp"
#include "Core/Entity/SplineEntity.hpp"
#include "Core/Entity/TableEntity.hpp"
#include "Core/Entity/TextEntity.hpp"
#include "Core/Entity/ToleranceEntity.hpp"
#include "Core/Entity/ViewportEntity.hpp"
#include "Core/Entity/WipeoutEntity.hpp"
#include "Core/Entity/XLineEntity.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace MiniCAD::SaSelectionExport
{
    inline constexpr int         kMinItems       = 1;
    inline constexpr int         kMaxItems       = 50;
    inline constexpr std::size_t kMaxScan        = 50000;
    inline constexpr std::size_t kMaxTextChars   = 120;
    inline constexpr int         kMaxAttribItems = 6;

    // 与 SaDwgTool SQLite `entities.handle` 完全一致：大写无 0x 前缀十六进制；0 返回空串。
    inline std::string FormatHandleHex(std::uint64_t handle)
    {
        if (handle == 0)
            return {};
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%llX", static_cast<unsigned long long>(handle));
        return std::string(buf);
    }

    namespace Detail
    {
        // 严格 RFC 3629 UTF-8 解码（含过长编码、UTF-16 代理项与 > U+10FFFF 码点校验），
        // 非法字节序列按 WHATWG 最大子段规则推进并返回 valid=false，由调用方替换为 U+FFFD。
        inline std::size_t DecodeUtf8Step(std::string_view s, std::size_t pos, bool& valid)
        {
            valid = false;
            const std::size_t n = s.size();
            if (pos >= n)
                return 0;
            const unsigned char c0 = static_cast<unsigned char>(s[pos]);
            if (c0 < 0x80u)
            {
                valid = true;
                return 1;
            }
            auto isCont = [](unsigned char c) { return (c & 0xC0u) == 0x80u; };
            if (c0 >= 0xC2u && c0 <= 0xDFu)
            {
                if (pos + 1 < n && isCont(static_cast<unsigned char>(s[pos + 1])))
                {
                    valid = true;
                    return 2;
                }
                return 1;
            }
            if (c0 >= 0xE0u && c0 <= 0xEFu)
            {
                if (pos + 1 >= n)
                    return 1;
                const unsigned char c1 = static_cast<unsigned char>(s[pos + 1]);
                const bool c1Ok = (c0 == 0xE0u) ? (c1 >= 0xA0u && c1 <= 0xBFu)
                                : (c0 == 0xEDu) ? (c1 >= 0x80u && c1 <= 0x9Fu)
                                                : isCont(c1);
                if (!c1Ok)
                    return 1;
                if (pos + 2 >= n || !isCont(static_cast<unsigned char>(s[pos + 2])))
                    return 2;
                valid = true;
                return 3;
            }
            if (c0 >= 0xF0u && c0 <= 0xF4u)
            {
                if (pos + 1 >= n)
                    return 1;
                const unsigned char c1 = static_cast<unsigned char>(s[pos + 1]);
                const bool c1Ok = (c0 == 0xF0u) ? (c1 >= 0x90u && c1 <= 0xBFu)
                                : (c0 == 0xF4u) ? (c1 >= 0x80u && c1 <= 0x8Fu)
                                                : isCont(c1);
                if (!c1Ok)
                    return 1;
                if (pos + 2 >= n || !isCont(static_cast<unsigned char>(s[pos + 2])))
                    return 2;
                if (pos + 3 >= n || !isCont(static_cast<unsigned char>(s[pos + 3])))
                    return 3;
                valid = true;
                return 4;
            }
            return 1;
        }
    }

    // 完整 RFC 8259 JSON 字符串转义：在入口统一校验 UTF-8 非法字节序列（含过长编码、代理项与越界码点）
    // 并替换为 U+FFFD，保证任意输入都产出严格合法的 UTF-8 JSON 字符串。
    inline std::string JsonQuote(std::string_view s)
    {
        std::string out;
        out.reserve(s.size() + 8);
        out.push_back('"');
        std::size_t i = 0;
        const std::size_t n = s.size();
        while (i < n)
        {
            bool valid = false;
            const std::size_t step = Detail::DecodeUtf8Step(s, i, valid);
            if (!valid)
            {
                out.append("\xEF\xBF\xBD", 3);
                i += (step > 0 ? step : 1);
                continue;
            }
            if (step == 1)
            {
                const unsigned char c = static_cast<unsigned char>(s[i]);
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
            else
            {
                out.append(s.data() + i, step);
            }
            i += step;
        }
        out.push_back('"');
        return out;
    }

    inline std::string FormatNumber(double v, int precision = 2)
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
            while (!s.empty() && s.back() == '0')
                s.pop_back();
            if (!s.empty() && s.back() == '.')
                s.pop_back();
        }
        if (s.empty() || s == "-0")
            return "0";
        return s;
    }

    // UTF-8 安全截断：按 Unicode 码点计数至多保留 maxChars 个完整字符，绝不从多字节序列中间截断，
    // 非法 UTF-8 序列统一替换为 U+FFFD，确保输出可被严格 JSON/UTF-8 解码器安全读取。
    inline std::string TruncateUtf8(std::string_view s, std::size_t maxChars = kMaxTextChars)
    {
        if (maxChars == 0 || s.empty())
            return {};
        std::string out;
        out.reserve(std::min<std::size_t>(s.size(), maxChars * 4));
        std::size_t chars = 0;
        std::size_t i = 0;
        const std::size_t n = s.size();
        while (i < n && chars < maxChars)
        {
            bool valid = false;
            const std::size_t step = Detail::DecodeUtf8Step(s, i, valid);
            if (!valid)
            {
                out.append("\xEF\xBF\xBD", 3);
                i += (step > 0 ? step : 1);
                ++chars;
                continue;
            }
            out.append(s.data() + i, step);
            i += step;
            ++chars;
        }
        return out;
    }

    inline std::string TrimWhitespace(std::string_view sv)
    {
        std::size_t start = 0;
        std::size_t end = sv.size();
        auto isSpace = [](char c) {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0';
        };
        while (start < end && isSpace(sv[start]))
            ++start;
        while (end > start && isSpace(sv[end - 1]))
            --end;
        return std::string(sv.substr(start, end - start));
    }

    inline std::string StripMTextCodes(const std::string& raw)
    {
        if (raw.empty())
            return {};
        return ParseMTextFormat(raw, 1.0).PlainText();
    }

    inline bool IsFiniteBox2D(const AABB& b)
    {
        return std::isfinite(b.Min.x) && std::isfinite(b.Min.y) &&
               std::isfinite(b.Max.x) && std::isfinite(b.Max.y) &&
               b.Min.x <= b.Max.x && b.Min.y <= b.Max.y &&
               std::abs(b.Min.x) < 1e15 && std::abs(b.Max.x) < 1e15 &&
               std::abs(b.Min.y) < 1e15 && std::abs(b.Max.y) < 1e15;
    }

    // 实体类型大写名（严格保证派生类先于基类判定：TableEntity → MTextEntity，
    // WipeoutEntity → PolylineEntity，MeshEntity → Solid3DEntity）。
    inline std::string EntityTypeName(const Entity& e)
    {
        if (e.IsKindOf<TableEntity>())     return "TABLE";
        if (e.IsKindOf<WipeoutEntity>())   return "WIPEOUT";
        if (e.IsKindOf<LineEntity>())      return "LINE";
        if (e.IsKindOf<PolylineEntity>())  return "POLYLINE";
        if (e.IsKindOf<CircleEntity>())    return "CIRCLE";
        if (e.IsKindOf<ArcEntity>())       return "ARC";
        if (e.IsKindOf<EllipseEntity>())   return "ELLIPSE";
        if (e.IsKindOf<SplineEntity>())    return "SPLINE";
        if (e.IsKindOf<TextEntity>())      return "TEXT";
        if (e.IsKindOf<MTextEntity>())     return "MTEXT";
        if (e.IsKindOf<InsertEntity>())    return "INSERT";
        if (e.IsKindOf<DimensionEntity>()) return "DIMENSION";
        if (e.IsKindOf<HatchEntity>())     return "HATCH";
        if (e.IsKindOf<MLeaderEntity>())   return "MLEADER";
        if (e.IsKindOf<LeaderEntity>())    return "LEADER";
        if (e.IsKindOf<SolidEntity>())     return "SOLID";
        if (e.IsKindOf<PointEntity>())     return "POINT";
        if (e.IsKindOf<ImageEntity>())     return "IMAGE";
        if (e.IsKindOf<RectangleEntity>()) return "RECTANGLE";
        if (e.IsKindOf<RayEntity>())       return "RAY";
        if (e.IsKindOf<XLineEntity>())     return "XLINE";
        if (e.IsKindOf<ViewportEntity>())  return "VIEWPORT";
        if (e.IsKindOf<OleFrameEntity>())  return "OLEFRAME";
        if (e.IsKindOf<MLineEntity>())     return "MLINE";
        if (e.IsKindOf<ShapeEntity>())     return "SHAPE";
        if (e.IsKindOf<ToleranceEntity>()) return "TOLERANCE";
        if (e.IsKindOf<Face3DEntity>())    return "FACE3D";
        if (e.IsKindOf<MeshEntity>())      return "MESH";
        if (e.IsKindOf<RegionEntity>())    return "REGION";
        if (e.IsKindOf<Solid3DEntity>())   return "SOLID3D";
        if (e.IsKindOf<AttribEntity>())    return "ATTRIB";
        if (e.IsKindOf<AttDefEntity>())    return "ATTDEF";
        const RuntimeTypeInfo* ti = e.GetTypeInfo();
        if (ti && ti->Name && ti->Name[0] != '\0')
            return std::string(ti->Name);
        return "UNKNOWN";
    }

    namespace Detail
    {
        inline constexpr double kPi       = 3.14159265358979323846;
        inline constexpr double kRadToDeg = 180.0 / kPi;

        inline std::string FormatDimensionText(const DimensionEntity& d)
        {
            const std::string& ov = d.GetTextOverride();
            if (!ov.empty() && ov.find("<>") == std::string::npos)
                return TrimWhitespace(StripMTextCodes(ov));

            char buf[64];
            const double val = d.Measurement();
            if (!std::isfinite(val))
                return {};
            switch (d.GetType())
            {
            case DimType::Angular:
                std::snprintf(buf, sizeof(buf), "%.1f\xC2\xB0", val * kRadToDeg);
                break;
            case DimType::Radius:
            case DimType::JoggedRadius:
                if (std::abs(val - std::round(val)) < 1e-3)
                    std::snprintf(buf, sizeof(buf), "R%.0f", std::round(val));
                else
                    std::snprintf(buf, sizeof(buf), "R%.2f", val);
                break;
            case DimType::Diameter:
                if (std::abs(val - std::round(val)) < 1e-3)
                    std::snprintf(buf, sizeof(buf), "\xE2\x8C\x80%.0f", std::round(val));
                else
                    std::snprintf(buf, sizeof(buf), "\xE2\x8C\x80%.2f", val);
                break;
            case DimType::ArcLength:
                if (std::abs(val - std::round(val)) < 1e-3)
                    std::snprintf(buf, sizeof(buf), "\xE2\x8C\x92%.0f", std::round(val));
                else
                    std::snprintf(buf, sizeof(buf), "\xE2\x8C\x92%.2f", val);
                break;
            default:
                if (std::abs(val - std::round(val)) < 1e-3)
                    std::snprintf(buf, sizeof(buf), "%.0f", std::round(val));
                else
                    std::snprintf(buf, sizeof(buf), "%.2f", val);
                break;
            }
            std::string num(buf);
            if (!ov.empty())
            {
                std::string s = StripMTextCodes(ov);
                const std::size_t pos = s.find("<>");
                if (pos != std::string::npos)
                    s.replace(pos, 2, num);
                return TrimWhitespace(s);
            }
            return num;
        }

        inline std::string ExtractEntityText(const Entity& e)
        {
            if (e.IsKindOf<TableEntity>())
            {
                const auto& tbl = static_cast<const TableEntity&>(e);
                std::string combined;
                int picked = 0;
                for (std::size_t r = 0; r < tbl.RowCount() && picked < kMaxAttribItems; ++r)
                {
                    for (std::size_t c = 0; c < tbl.ColCount() && picked < kMaxAttribItems; ++c)
                    {
                        const std::string cell = TrimWhitespace(StripMTextCodes(tbl.Cell(r, c).Text));
                        if (cell.empty())
                            continue;
                        if (!combined.empty())
                            combined += " | ";
                        combined += cell;
                        ++picked;
                        if (combined.size() >= kMaxTextChars * 4)
                            break;
                    }
                }
                return TruncateUtf8(combined, kMaxTextChars);
            }
            if (e.IsKindOf<TextEntity>())
            {
                return TruncateUtf8(TrimWhitespace(static_cast<const TextEntity&>(e).GetText()), kMaxTextChars);
            }
            if (e.IsKindOf<MTextEntity>())
            {
                const auto& mt = static_cast<const MTextEntity&>(e);
                return TruncateUtf8(TrimWhitespace(StripMTextCodes(mt.GetText())), kMaxTextChars);
            }
            if (e.IsKindOf<DimensionEntity>())
            {
                return TruncateUtf8(FormatDimensionText(static_cast<const DimensionEntity&>(e)), kMaxTextChars);
            }
            if (e.IsKindOf<MLeaderEntity>())
            {
                const auto& ml = static_cast<const MLeaderEntity&>(e);
                if (ml.GetContentType() == MLeaderContent::MText && !ml.GetText().empty())
                    return TruncateUtf8(TrimWhitespace(StripMTextCodes(ml.GetText())), kMaxTextChars);
                return {};
            }
            if (e.IsKindOf<LeaderEntity>())
            {
                const auto& ld = static_cast<const LeaderEntity&>(e);
                if (!ld.GetText().empty())
                    return TruncateUtf8(TrimWhitespace(StripMTextCodes(ld.GetText())), kMaxTextChars);
                return {};
            }
            if (e.IsKindOf<InsertEntity>())
            {
                const auto& ins = static_cast<const InsertEntity&>(e);
                std::string combined;
                int picked = 0;
                for (const auto& a : ins.GetAttribs())
                {
                    if (!a || a->IsInvisible())
                        continue;
                    const std::string val = TrimWhitespace(a->GetText());
                    if (val.empty())
                        continue;
                    const std::string tag = TrimWhitespace(a->GetTag());
                    if (!combined.empty())
                        combined += "; ";
                    if (!tag.empty())
                    {
                        combined += tag;
                        combined.push_back('=');
                    }
                    combined += val;
                    ++picked;
                    if (picked >= kMaxAttribItems || combined.size() >= kMaxTextChars * 4)
                        break;
                }
                return TruncateUtf8(combined, kMaxTextChars);
            }
            if (e.IsKindOf<AttribBase>())
            {
                const auto& ab = static_cast<const AttribBase&>(e);
                return TruncateUtf8(TrimWhitespace(ab.GetText()), kMaxTextChars);
            }
            return {};
        }

        struct GeomMetrics
        {
            std::optional<double> Length;
            std::optional<double> Area;
            std::optional<double> Radius;
        };

        inline double PolylineLengthAndArea(const Polyline& pl, bool& closedOut, double& areaOut)
        {
            closedOut = false;
            areaOut = 0.0;
            const auto& pts = pl.Points;
            if (pts.size() < 2)
                return 0.0;
            const double dClose = std::hypot(pts.back().x - pts.front().x, pts.back().y - pts.front().y);
            closedOut = (pts.size() >= 3 && dClose < 1e-4);

            double totalLen = 0.0;
            double area2 = 0.0;
            double bulgeArea = 0.0;
            const std::size_t segs = pts.size() - 1;
            for (std::size_t i = 0; i < segs; ++i)
            {
                const auto& a = pts[i];
                const auto& b = pts[i + 1];
                const double chord = std::hypot(b.x - a.x, b.y - a.y);
                const double bulge = (i < pl.Bulges.size()) ? pl.Bulges[i] : 0.0;
                if (std::abs(bulge) > 1e-9)
                {
                    const double theta = 4.0 * std::atan(bulge);
                    const double s = std::sin(std::abs(theta) * 0.5);
                    if (s > 1e-9)
                    {
                        const double r = chord / (2.0 * s);
                        totalLen += r * std::abs(theta);
                        const double seg = 0.5 * r * r * (std::abs(theta) - std::sin(std::abs(theta)));
                        bulgeArea += (bulge > 0.0 ? seg : -seg);
                    }
                    else
                    {
                        totalLen += chord;
                    }
                }
                else
                {
                    totalLen += chord;
                }
                if (closedOut)
                    area2 += a.x * b.y - b.x * a.y;
            }
            if (closedOut)
                areaOut = std::abs(0.5 * area2 + bulgeArea);
            return totalLen;
        }

        inline std::optional<double> ComputeHatchAreaSafe(const HatchEntity& hatch)
        {
            return ComputeHatchNetArea(hatch, 200000);
        }

        inline GeomMetrics ExtractGeomMetrics(const Entity& e)
        {
            GeomMetrics g;
            if (e.IsKindOf<LineEntity>())
            {
                const auto& l = static_cast<const LineEntity&>(e).GetLine();
                const double len = (l.End - l.Start).Length();
                if (std::isfinite(len))
                    g.Length = len;
            }
            else if (e.IsKindOf<WipeoutEntity>())
            {
                const auto& pl = static_cast<const WipeoutEntity&>(e).GetPolyline();
                bool closed = false;
                double area = 0.0;
                const double len = PolylineLengthAndArea(pl, closed, area);
                if (std::isfinite(len))
                    g.Length = len;
                if (closed && std::isfinite(area))
                    g.Area = area;
            }
            else if (e.IsKindOf<PolylineEntity>())
            {
                const auto& pl = static_cast<const PolylineEntity&>(e).GetPolyline();
                bool closed = false;
                double area = 0.0;
                const double len = PolylineLengthAndArea(pl, closed, area);
                if (std::isfinite(len))
                    g.Length = len;
                if (closed && std::isfinite(area))
                    g.Area = area;
            }
            else if (e.IsKindOf<CircleEntity>())
            {
                const double r = static_cast<const CircleEntity&>(e).GetCircle().Radius;
                if (std::isfinite(r) && r >= 0.0)
                {
                    g.Radius = r;
                    g.Area   = kPi * r * r;
                }
            }
            else if (e.IsKindOf<ArcEntity>())
            {
                const auto& a = static_cast<const ArcEntity&>(e).GetArc();
                if (std::isfinite(a.Radius) && a.Radius >= 0.0)
                {
                    g.Radius = a.Radius;
                    const double len = a.Radius * std::abs(a.SweepAngle());
                    if (std::isfinite(len))
                        g.Length = len;
                }
            }
            else if (e.IsKindOf<EllipseEntity>())
            {
                const auto& el = static_cast<const EllipseEntity&>(e).GetEllipse();
                if (el.IsFull())
                {
                    const double area = kPi * std::abs(el.RadiusX * el.RadiusY);
                    if (std::isfinite(area))
                        g.Area = area;
                }
            }
            else if (e.IsKindOf<HatchEntity>())
            {
                g.Area = ComputeHatchAreaSafe(static_cast<const HatchEntity&>(e));
            }
            else if (e.IsKindOf<RectangleEntity>())
            {
                const auto& r = static_cast<const RectangleEntity&>(e).GetRectangle();
                const double w = (r.P2 - r.P1).Length();
                const double h = (r.P4 - r.P1).Length();
                if (std::isfinite(w) && std::isfinite(h))
                {
                    g.Length = 2.0 * (w + h);
                    g.Area   = w * h;
                }
            }
            return g;
        }

        inline void AppendBoundsJson(std::string& out, const AABB& b)
        {
            out += "{\"minX\":";
            out += FormatNumber(b.Min.x, 3);
            out += ",\"minY\":";
            out += FormatNumber(b.Min.y, 3);
            out += ",\"maxX\":";
            out += FormatNumber(b.Max.x, 3);
            out += ",\"maxY\":";
            out += FormatNumber(b.Max.y, 3);
            out.push_back('}');
        }

        inline void AppendOptMetric(std::string& out, const std::optional<double>& v)
        {
            if (v.has_value() && std::isfinite(*v))
                out += FormatNumber(*v, 2);
            else
                out += "null";
        }

        inline std::uint64_t SplitMix64(std::uint64_t x)
        {
            x += 0x9E3779B97F4A7C15ull;
            x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
            x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
            return x ^ (x >> 31);
        }
    }

    // 计算当前主文档 + 活动布局 + 选择集的稳定非 0 版本号（无文档时返回 0）。
    // 使用 Document::InstanceID() 杜绝文档关闭重建后的指针复用 ABA 问题。
    inline std::uint32_t ComputeEmbedSelectionVersion(const Document* doc, const Editor* editor)
    {
        if (!doc)
            return 0u;
        const std::uint64_t docId   = doc->InstanceID();
        const std::uint64_t layIdx  = static_cast<std::uint64_t>(static_cast<std::uint32_t>(doc->ActiveLayout()));
        const std::uint64_t layKey  = doc->ActiveLayoutOwnerKey();
        const std::uint64_t selVer  = editor ? editor->SelectionVersion() : 0ull;

        std::uint64_t h = Detail::SplitMix64(docId);
        h ^= Detail::SplitMix64((layIdx << 32) ^ layKey ^ 0xA5A5A5A55A5A5A5Aull);
        h ^= Detail::SplitMix64(selVer + 1ull);
        const std::uint32_t ver = static_cast<std::uint32_t>(h ^ (h >> 32));
        return ver == 0u ? 1u : ver;
    }

    inline std::string EmptySelectionJson()
    {
        return "{\"count\":0,\"scannedCount\":0,\"truncated\":false,\"statsTruncated\":false,\"bounds\":null,\"typeCounts\":{},\"items\":[]}";
    }

    // 导出选中图元摘要 JSON。
    // activeDoc 非空时仅纳入当前活动布局可见实体（兼顾模型空间与各图纸空间布局）；为空时纳入场景中全部命中实体。
    // maxScan 限制统计与明细的扫描上限（默认 kMaxScan），同时输出准确总选中数 count、实际扫描数 scannedCount
    // 与 statsTruncated（scannedCount < count），truncated 表示 items.size() < count。
    inline std::string ExportSelectionJson(const Scene& scene,
                                           const std::unordered_set<Object::ObjectID>& selectionIds,
                                           const CadSource* cadSource = nullptr,
                                           int maxItems = 20,
                                           const Document* activeDoc = nullptr,
                                           std::size_t maxScan = kMaxScan)
    {
        const int limit = std::clamp(maxItems, kMinItems, kMaxItems);
        if (selectionIds.empty())
            return EmptySelectionJson();

        // 收集全部在场景中存在且属于当前活动布局的有效实体指针，以获得准确总选中数 count；
        // 当超过扫描上限时仅按 ObjectID 升序稳定抽取前 scanLimit 个计算类型统计、包围盒与明细。
        std::vector<const Entity*> entities;
        entities.reserve(selectionIds.size());
        for (Object::ObjectID id : selectionIds)
        {
            const Object* o = scene.GetEntity(id);
            if (!o || !o->IsKindOf<Entity>())
                continue;
            const auto* e = static_cast<const Entity*>(o);
            if (activeDoc && !activeDoc->IsVisibleInActiveLayout(e->GetAttr()))
                continue;
            entities.push_back(e);
        }

        if (entities.empty())
            return EmptySelectionJson();

        const std::size_t totalCount = entities.size();
        const std::size_t scanLimit  = maxScan > 0 ? maxScan : kMaxScan;
        const std::size_t scannedCount = std::min<std::size_t>(totalCount, scanLimit);
        const bool statsTruncated = (scannedCount < totalCount);

        auto byId = [](const Entity* a, const Entity* b) { return a->GetID() < b->GetID(); };
        if (statsTruncated)
        {
            std::nth_element(entities.begin(),
                             entities.begin() + static_cast<std::ptrdiff_t>(scannedCount),
                             entities.end(),
                             byId);
            entities.resize(scannedCount);
        }
        std::sort(entities.begin(), entities.end(), byId);

        std::map<std::string, std::size_t> typeCounts;
        AABB unionBounds = AABB::Empty();
        bool hasUnionBounds = false;

        for (const Entity* e : entities)
        {
            ++typeCounts[EntityTypeName(*e)];
            if (!e->IsBoundless())
            {
                const AABB b = e->GetBoundingBox();
                if (IsFiniteBox2D(b))
                {
                    unionBounds.Expand(b.Min);
                    unionBounds.Expand(b.Max);
                    hasUnionBounds = true;
                }
            }
        }

        const std::size_t emitCount = std::min<std::size_t>(scannedCount, static_cast<std::size_t>(limit));
        const bool truncated = (emitCount < totalCount);
        std::string out;
        out.reserve(256 + emitCount * 220);

        out += "{\"count\":";
        out += std::to_string(totalCount);
        out += ",\"scannedCount\":";
        out += std::to_string(scannedCount);
        out += ",\"truncated\":";
        out += truncated ? "true" : "false";
        out += ",\"statsTruncated\":";
        out += statsTruncated ? "true" : "false";
        out += ",\"bounds\":";
        if (hasUnionBounds && IsFiniteBox2D(unionBounds))
            Detail::AppendBoundsJson(out, unionBounds);
        else
            out += "null";

        out += ",\"typeCounts\":{";
        bool firstType = true;
        for (const auto& [typeName, cnt] : typeCounts)
        {
            if (!firstType)
                out.push_back(',');
            firstType = false;
            out += JsonQuote(typeName);
            out.push_back(':');
            out += std::to_string(cnt);
        }
        out += "},\"items\":[";

        const LayerManager& layers = scene.GetLayerManager();
        for (std::size_t i = 0; i < emitCount; ++i)
        {
            if (i > 0)
                out.push_back(',');
            const Entity& e = *entities[i];
            const std::uint64_t handleVal = LookupCadEntityHandle(cadSource, e.GetID());
            const std::string handleHex = FormatHandleHex(handleVal);
            const std::string typeName  = EntityTypeName(e);
            const Layer* lyr = layers.GetLayer(e.GetLayerID());
            const std::string layerName = lyr ? lyr->GetName() : std::string("0");
            const std::string text = Detail::ExtractEntityText(e);
            const Detail::GeomMetrics geom = Detail::ExtractGeomMetrics(e);

            out += "{\"objectId\":";
            out += std::to_string(e.GetID());
            out += ",\"handle\":";
            if (!handleHex.empty())
                out += JsonQuote(handleHex);
            else
                out += "null";
            out += ",\"type\":";
            out += JsonQuote(typeName);
            out += ",\"layer\":";
            out += JsonQuote(layerName);
            out += ",\"blockName\":";
            if (e.IsKindOf<InsertEntity>())
            {
                const std::string& bn = static_cast<const InsertEntity&>(e).GetBlockName();
                if (!bn.empty())
                    out += JsonQuote(bn);
                else
                    out += "null";
            }
            else
            {
                out += "null";
            }
            out += ",\"text\":";
            if (!text.empty())
                out += JsonQuote(text);
            else
                out += "null";
            out += ",\"length\":";
            Detail::AppendOptMetric(out, geom.Length);
            out += ",\"area\":";
            Detail::AppendOptMetric(out, geom.Area);
            out += ",\"radius\":";
            Detail::AppendOptMetric(out, geom.Radius);
            out += ",\"bounds\":";
            if (!e.IsBoundless())
            {
                const AABB eb = e.GetBoundingBox();
                if (IsFiniteBox2D(eb))
                    Detail::AppendBoundsJson(out, eb);
                else
                    out += "null";
            }
            else
            {
                out += "null";
            }
            out.push_back('}');
        }
        out += "]}";
        return out;
    }
}
