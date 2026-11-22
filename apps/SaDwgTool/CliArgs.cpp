#include "CliArgs.h"
#include <cmath>
#include <cstdlib>
#include <filesystem>

namespace MiniCAD::SaTool
{
    namespace
    {
        bool ParseDoubleStrict(const std::string& s, double& out)
        {
            if (s.empty()) return false;
            char* end = nullptr;
            const double v = std::strtod(s.c_str(), &end);
            if (end == s.c_str() || *end != '\0' || !std::isfinite(v))
                return false;
            out = v;
            return true;
        }

        bool ParseIntStrict(const std::string& s, int& out)
        {
            if (s.empty()) return false;
            char* end = nullptr;
            const long v = std::strtol(s.c_str(), &end, 10);
            if (end == s.c_str() || *end != '\0')
                return false;
            out = static_cast<int>(v);
            return true;
        }

        bool NeedArgs(const std::vector<std::string>& args, size_t i, size_t count,
                      const std::string& opt, std::string& errCode, std::string& errMsg)
        {
            if (i + count >= args.size())
            {
                errCode = "INVALID_ARGS";
                errMsg  = "参数 " + opt + " 缺少必需的值";
                return false;
            }
            return true;
        }

        bool ParseBbox4(const std::vector<std::string>& args, size_t& i,
                        std::array<double, 4>& bbox, std::string& errCode, std::string& errMsg)
        {
            if (!NeedArgs(args, i, 4, "--bbox", errCode, errMsg))
                return false;
            for (int k = 0; k < 4; ++k)
            {
                if (!ParseDoubleStrict(args[i + 1 + static_cast<size_t>(k)], bbox[static_cast<size_t>(k)]))
                {
                    errCode = "INVALID_BBOX";
                    errMsg  = "--bbox 坐标必须是有效数字";
                    return false;
                }
            }
            i += 4;
            if (bbox[2] <= bbox[0] || bbox[3] <= bbox[1])
            {
                errCode = "INVALID_BBOX";
                errMsg  = "--bbox 窗口无效：要求 xmax > xmin 且 ymax > ymin";
                return false;
            }
            return true;
        }

        std::string DefaultJsonPathForPng(const std::string& pngPath)
        {
            const size_t dot = pngPath.find_last_of('.');
            const size_t sep = pngPath.find_last_of("/\\");
            if (dot != std::string::npos && (sep == std::string::npos || dot > sep))
                return pngPath.substr(0, dot) + ".json";
            return pngPath + ".json";
        }
    }

    std::string BuildUsageText()
    {
        return
            "SaDwgTool - 原生 64 位无头 DWG/DXF 解析、渲染与三轨比对引擎\n"
            "用法:\n"
            "  SaDwgTool --version\n"
            "  SaDwgTool export-db <in.dwg|dxf> <out.sqlite> [--reuse-if-fresh] [--force] [--no-block-entities] [--fonts-dir <dir>] [--pretty] [--verbose]\n"
            "  SaDwgTool render <in.dwg|dxf> <out.png> [--compare <b.dwg|dxf>] [--offset <dx> <dy>] [--layout <name>] [--layout-b <name>]\n"
            "                   [--bbox <xmin> <ymin> <xmax> <ymax>] [--mm-per-px <f>] [--max-px <n>] [--bg white|black]\n"
            "                   [--line-width-px <f>] [--include-layers <re>] [--exclude-layers <re>] [--exclude-annotations]\n"
            "                   [--exclude-frames] [--fonts-dir <dir>] [--pretty] [--verbose]\n"
            "  SaDwgTool diff <a.dwg|dxf> <b.dwg|dxf> [--out-png <png>] [--out-json <json>] [--out-db <sqlite>]\n"
            "                 [--layout <name>] [--layout-b <name>] [--bbox <xmin> <ymin> <xmax> <ymax>]\n"
            "                 [--auto-align] [--offset <dx> <dy>] [--align-layers-a <re>] [--align-layers-b <re>]\n"
            "                 [--align-tol <mm>] [--align-min-span <mm>] [--mm-per-px <f>] [--max-px <n>]\n"
            "                 [--tolerance-mm <f>] [--min-region-mm <f>] [--region-gap-mm <f>] [--max-region-mm <f>]\n"
            "                 [--include-layers <re>] [--exclude-layers <re>] [--include-annotations] [--exclude-frames]\n"
            "                 [--text-match-tol-mm <f>] [--tch-match-tol-mm <f>] [--no-region-boxes] [--fonts-dir <dir>] [--pretty] [--verbose]\n";
    }

    bool ParseCliArgs(const std::vector<std::string>& args, ParsedCommand& out,
                      std::string& errorCode, std::string& errorMsg)
    {
        out = {};
        if (args.empty())
        {
            errorCode = "MISSING_COMMAND";
            errorMsg  = "缺少子命令（支持 export-db / render / diff / --version）";
            return false;
        }

        const std::string& cmd = args[0];
        if (cmd == "--version" || cmd == "-v" || cmd == "version")
        {
            out.Kind = CommandKind::Version;
            return true;
        }
        if (cmd == "--help" || cmd == "-h" || cmd == "help")
        {
            out.Kind = CommandKind::Help;
            return true;
        }

        if (cmd == "export-db")
        {
            out.Kind = CommandKind::ExportDb;
            std::vector<std::string> pos;
            for (size_t i = 1; i < args.size(); ++i)
            {
                const std::string& a = args[i];
                if (a == "--reuse-if-fresh")       out.ExportDb.ReuseIfFresh = true;
                else if (a == "--force")           out.ExportDb.Force = true;
                else if (a == "--no-block-entities") out.ExportDb.NoBlockEntities = true;
                else if (a == "--pretty")          { out.ExportDb.Pretty = true; out.Pretty = true; }
                else if (a == "--verbose")         { out.ExportDb.Verbose = true; out.Verbose = true; }
                else if (a == "--fonts-dir")
                {
                    if (!NeedArgs(args, i, 1, a, errorCode, errorMsg)) return false;
                    out.ExportDb.FontsDir = args[++i];
                }
                else if (!a.empty() && a[0] == '-')
                {
                    errorCode = "UNKNOWN_OPTION";
                    errorMsg  = "export-db 不支持参数：" + a;
                    return false;
                }
                else
                {
                    pos.push_back(a);
                }
            }
            if (pos.size() != 2)
            {
                errorCode = "INVALID_ARGS";
                errorMsg  = "export-db 需要 2 个位置参数：<in.dwg|dxf> <out.sqlite>";
                return false;
            }
            out.ExportDb.InputPath    = pos[0];
            out.ExportDb.OutputDbPath = pos[1];
            return true;
        }

        if (cmd == "render")
        {
            out.Kind = CommandKind::Render;
            std::vector<std::string> pos;
            for (size_t i = 1; i < args.size(); ++i)
            {
                const std::string& a = args[i];
                if (a == "--compare")
                {
                    if (!NeedArgs(args, i, 1, a, errorCode, errorMsg)) return false;
                    out.Render.ComparePath = args[++i];
                }
                else if (a == "--offset")
                {
                    if (!NeedArgs(args, i, 2, a, errorCode, errorMsg)) return false;
                    if (!ParseDoubleStrict(args[i + 1], out.Render.OffsetDx) ||
                        !ParseDoubleStrict(args[i + 2], out.Render.OffsetDy))
                    {
                        errorCode = "INVALID_OFFSET";
                        errorMsg  = "--offset 需要两个有效数字 dx dy";
                        return false;
                    }
                    i += 2;
                }
                else if (a == "--layout")
                {
                    if (!NeedArgs(args, i, 1, a, errorCode, errorMsg)) return false;
                    out.Render.LayoutName = args[++i];
                }
                else if (a == "--layout-b")
                {
                    if (!NeedArgs(args, i, 1, a, errorCode, errorMsg)) return false;
                    out.Render.LayoutNameB = args[++i];
                }
                else if (a == "--bbox")
                {
                    if (!ParseBbox4(args, i, out.Render.Bbox, errorCode, errorMsg)) return false;
                    out.Render.HasBbox = true;
                }
                else if (a == "--mm-per-px")
                {
                    if (!NeedArgs(args, i, 1, a, errorCode, errorMsg)) return false;
                    if (!ParseDoubleStrict(args[++i], out.Render.MmPerPx) || out.Render.MmPerPx <= 0.0)
                    {
                        errorCode = "INVALID_ARGS";
                        errorMsg  = "--mm-per-px 必须为正数";
                        return false;
                    }
                }
                else if (a == "--max-px")
                {
                    if (!NeedArgs(args, i, 1, a, errorCode, errorMsg)) return false;
                    if (!ParseIntStrict(args[++i], out.Render.MaxPx) || out.Render.MaxPx < 16 || out.Render.MaxPx > 8192)
                    {
                        errorCode = "INVALID_CANVAS";
                        errorMsg  = "--max-px 必须在 16..8192 之间";
                        return false;
                    }
                }
                else if (a == "--bg")
                {
                    if (!NeedArgs(args, i, 1, a, errorCode, errorMsg)) return false;
                    const std::string bg = args[++i];
                    if (bg == "white")      out.Render.DarkBg = false;
                    else if (bg == "black") out.Render.DarkBg = true;
                    else
                    {
                        errorCode = "INVALID_ARGS";
                        errorMsg  = "--bg 仅支持 white 或 black";
                        return false;
                    }
                }
                else if (a == "--line-width-px")
                {
                    if (!NeedArgs(args, i, 1, a, errorCode, errorMsg)) return false;
                    double w = 1.0;
                    if (!ParseDoubleStrict(args[++i], w) || w < 1.0 || w > 4.0)
                    {
                        errorCode = "INVALID_ARGS";
                        errorMsg  = "--line-width-px 必须在 1.0..4.0 之间";
                        return false;
                    }
                    out.Render.LineWidthPx = static_cast<float>(w);
                }
                else if (a == "--include-layers")
                {
                    if (!NeedArgs(args, i, 1, a, errorCode, errorMsg)) return false;
                    out.Render.IncludeLayersRegex = args[++i];
                }
                else if (a == "--exclude-layers")
                {
                    if (!NeedArgs(args, i, 1, a, errorCode, errorMsg)) return false;
                    out.Render.ExcludeLayersRegex = args[++i];
                }
                else if (a == "--exclude-annotations") out.Render.ExcludeAnnotations = true;
                else if (a == "--exclude-frames")      out.Render.ExcludeFrames = true;
                else if (a == "--fonts-dir")
                {
                    if (!NeedArgs(args, i, 1, a, errorCode, errorMsg)) return false;
                    out.Render.FontsDir = args[++i];
                }
                else if (a == "--pretty")  { out.Render.Pretty = true; out.Pretty = true; }
                else if (a == "--verbose") { out.Render.Verbose = true; out.Verbose = true; }
                else if (!a.empty() && a[0] == '-')
                {
                    errorCode = "UNKNOWN_OPTION";
                    errorMsg  = "render 不支持参数：" + a;
                    return false;
                }
                else
                {
                    pos.push_back(a);
                }
            }
            if (pos.size() != 2)
            {
                errorCode = "INVALID_ARGS";
                errorMsg  = "render 需要 2 个位置参数：<in.dwg|dxf> <out.png>";
                return false;
            }
            out.Render.InputPath     = pos[0];
            out.Render.OutputPngPath = pos[1];
            return true;
        }

        if (cmd == "diff")
        {
            out.Kind = CommandKind::Diff;
            std::vector<std::string> pos;
            for (size_t i = 1; i < args.size(); ++i)
            {
                const std::string& a = args[i];
                if (a == "--out-png")
                {
                    if (!NeedArgs(args, i, 1, a, errorCode, errorMsg)) return false;
                    out.Diff.OutPngPath = args[++i];
                }
                else if (a == "--out-json")
                {
                    if (!NeedArgs(args, i, 1, a, errorCode, errorMsg)) return false;
                    out.Diff.OutJsonPath     = args[++i];
                    out.Diff.OutJsonExplicit = true;
                }
                else if (a == "--out-db")
                {
                    if (!NeedArgs(args, i, 1, a, errorCode, errorMsg)) return false;
                    out.Diff.OutDbPath = args[++i];
                }
                else if (a == "--layout")
                {
                    if (!NeedArgs(args, i, 1, a, errorCode, errorMsg)) return false;
                    out.Diff.LayoutName = args[++i];
                }
                else if (a == "--layout-b")
                {
                    if (!NeedArgs(args, i, 1, a, errorCode, errorMsg)) return false;
                    out.Diff.LayoutNameB = args[++i];
                }
                else if (a == "--bbox")
                {
                    if (!ParseBbox4(args, i, out.Diff.Bbox, errorCode, errorMsg)) return false;
                    out.Diff.HasBbox = true;
                }
                else if (a == "--auto-align")
                {
                    out.Diff.AutoAlign = true;
                }
                else if (a == "--offset")
                {
                    if (!NeedArgs(args, i, 2, a, errorCode, errorMsg)) return false;
                    if (!ParseDoubleStrict(args[i + 1], out.Diff.OffsetDx) ||
                        !ParseDoubleStrict(args[i + 2], out.Diff.OffsetDy))
                    {
                        errorCode = "INVALID_OFFSET";
                        errorMsg  = "--offset 需要两个有效数字 dx dy";
                        return false;
                    }
                    out.Diff.HasOffset = true;
                    i += 2;
                }
                else if (a == "--align-layers-a")
                {
                    if (!NeedArgs(args, i, 1, a, errorCode, errorMsg)) return false;
                    out.Diff.AlignLayersA = args[++i];
                }
                else if (a == "--align-layers-b")
                {
                    if (!NeedArgs(args, i, 1, a, errorCode, errorMsg)) return false;
                    out.Diff.AlignLayersB = args[++i];
                }
                else if (a == "--align-tol")
                {
                    if (!NeedArgs(args, i, 1, a, errorCode, errorMsg)) return false;
                    if (!ParseDoubleStrict(args[++i], out.Diff.AlignTolMm) || out.Diff.AlignTolMm <= 0.0)
                    {
                        errorCode = "INVALID_ARGS"; errorMsg = "--align-tol 必须为正数"; return false;
                    }
                }
                else if (a == "--align-min-span")
                {
                    if (!NeedArgs(args, i, 1, a, errorCode, errorMsg)) return false;
                    if (!ParseDoubleStrict(args[++i], out.Diff.AlignMinSpanMm) || out.Diff.AlignMinSpanMm < 0.0)
                    {
                        errorCode = "INVALID_ARGS"; errorMsg = "--align-min-span 不能为负数"; return false;
                    }
                }
                else if (a == "--mm-per-px")
                {
                    if (!NeedArgs(args, i, 1, a, errorCode, errorMsg)) return false;
                    if (!ParseDoubleStrict(args[++i], out.Diff.MmPerPx) || out.Diff.MmPerPx <= 0.0)
                    {
                        errorCode = "INVALID_ARGS"; errorMsg = "--mm-per-px 必须为正数"; return false;
                    }
                }
                else if (a == "--max-px")
                {
                    if (!NeedArgs(args, i, 1, a, errorCode, errorMsg)) return false;
                    if (!ParseIntStrict(args[++i], out.Diff.MaxPx) || out.Diff.MaxPx < 16 || out.Diff.MaxPx > 8192)
                    {
                        errorCode = "INVALID_CANVAS"; errorMsg = "--max-px 必须在 16..8192 之间"; return false;
                    }
                }
                else if (a == "--tolerance-mm" || a == "--tolerance")
                {
                    if (!NeedArgs(args, i, 1, a, errorCode, errorMsg)) return false;
                    if (!ParseDoubleStrict(args[++i], out.Diff.ToleranceMm) || out.Diff.ToleranceMm < 0.0)
                    {
                        errorCode = "INVALID_ARGS"; errorMsg = "--tolerance-mm 不能为负数"; return false;
                    }
                }
                else if (a == "--min-region-mm")
                {
                    if (!NeedArgs(args, i, 1, a, errorCode, errorMsg)) return false;
                    if (!ParseDoubleStrict(args[++i], out.Diff.MinRegionMm) || out.Diff.MinRegionMm < 0.0)
                    {
                        errorCode = "INVALID_ARGS"; errorMsg = "--min-region-mm 不能为负数"; return false;
                    }
                }
                else if (a == "--region-gap-mm")
                {
                    if (!NeedArgs(args, i, 1, a, errorCode, errorMsg)) return false;
                    if (!ParseDoubleStrict(args[++i], out.Diff.RegionGapMm) || out.Diff.RegionGapMm <= 0.0)
                    {
                        errorCode = "INVALID_ARGS"; errorMsg = "--region-gap-mm 必须为正数"; return false;
                    }
                }
                else if (a == "--max-region-mm")
                {
                    if (!NeedArgs(args, i, 1, a, errorCode, errorMsg)) return false;
                    if (!ParseDoubleStrict(args[++i], out.Diff.MaxRegionMm) || out.Diff.MaxRegionMm <= 0.0)
                    {
                        errorCode = "INVALID_ARGS"; errorMsg = "--max-region-mm 必须为正数"; return false;
                    }
                }
                else if (a == "--include-layers")
                {
                    if (!NeedArgs(args, i, 1, a, errorCode, errorMsg)) return false;
                    out.Diff.IncludeLayersRegex = args[++i];
                }
                else if (a == "--exclude-layers")
                {
                    if (!NeedArgs(args, i, 1, a, errorCode, errorMsg)) return false;
                    out.Diff.ExcludeLayersRegex = args[++i];
                }
                else if (a == "--include-annotations") out.Diff.IncludeAnnotations = true;
                else if (a == "--exclude-frames")      out.Diff.ExcludeFrames = true;
                else if (a == "--no-region-boxes")     out.Diff.NoRegionBoxes = true;
                else if (a == "--text-match-tol-mm")
                {
                    if (!NeedArgs(args, i, 1, a, errorCode, errorMsg)) return false;
                    if (!ParseDoubleStrict(args[++i], out.Diff.TextMatchTolMm) || out.Diff.TextMatchTolMm < 0.0)
                    {
                        errorCode = "INVALID_ARGS"; errorMsg = "--text-match-tol-mm 不能为负数"; return false;
                    }
                }
                else if (a == "--tch-match-tol-mm")
                {
                    if (!NeedArgs(args, i, 1, a, errorCode, errorMsg)) return false;
                    if (!ParseDoubleStrict(args[++i], out.Diff.TchMatchTolMm) || out.Diff.TchMatchTolMm < 0.0)
                    {
                        errorCode = "INVALID_ARGS"; errorMsg = "--tch-match-tol-mm 不能为负数"; return false;
                    }
                }
                else if (a == "--fonts-dir")
                {
                    if (!NeedArgs(args, i, 1, a, errorCode, errorMsg)) return false;
                    out.Diff.FontsDir = args[++i];
                }
                else if (a == "--pretty")  { out.Diff.Pretty = true; out.Pretty = true; }
                else if (a == "--verbose") { out.Diff.Verbose = true; out.Verbose = true; }
                else if (!a.empty() && a[0] == '-')
                {
                    errorCode = "UNKNOWN_OPTION";
                    errorMsg  = "diff 不支持参数：" + a;
                    return false;
                }
                else
                {
                    pos.push_back(a);
                }
            }
            if (pos.size() != 2)
            {
                errorCode = "INVALID_ARGS";
                errorMsg  = "diff 需要 2 个位置参数：<a.dwg|dxf> <b.dwg|dxf>";
                return false;
            }
            out.Diff.InputPathA = pos[0];
            out.Diff.InputPathB = pos[1];
            if (!out.Diff.OutJsonExplicit && !out.Diff.OutPngPath.empty())
                out.Diff.OutJsonPath = DefaultJsonPathForPng(out.Diff.OutPngPath);
            return true;
        }

        errorCode = "UNKNOWN_COMMAND";
        errorMsg  = "未知子命令：" + cmd + "（支持 export-db / render / diff / --version）";
        return false;
    }
}
