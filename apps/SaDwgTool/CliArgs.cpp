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

        std::string TrimAsciiWs(const std::string& s)
        {
            size_t b = 0;
            while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
                ++b;
            size_t e = s.size();
            while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
                --e;
            return s.substr(b, e - b);
        }

        bool ParseBbox(const std::vector<std::string>& args, size_t& i,
                       std::array<double, 4>& bbox, std::string& errCode, std::string& errMsg)
        {
            if (!NeedArgs(args, i, 1, "--bbox", errCode, errMsg))
                return false;

            const std::string& first = args[i + 1];
            if (first.find(',') != std::string::npos)
            {
                std::vector<std::string> parts;
                size_t start = 0;
                while (true)
                {
                    const size_t comma = first.find(',', start);
                    if (comma == std::string::npos)
                    {
                        parts.push_back(TrimAsciiWs(first.substr(start)));
                        break;
                    }
                    parts.push_back(TrimAsciiWs(first.substr(start, comma - start)));
                    start = comma + 1;
                }
                if (parts.size() != 4)
                {
                    errCode = "INVALID_BBOX";
                    errMsg  = "--bbox 逗号分隔格式需要恰好 4 个数字：xmin,ymin,xmax,ymax";
                    return false;
                }
                for (int k = 0; k < 4; ++k)
                {
                    if (!ParseDoubleStrict(parts[static_cast<size_t>(k)], bbox[static_cast<size_t>(k)]))
                    {
                        errCode = "INVALID_BBOX";
                        errMsg  = "--bbox 坐标必须是有效数字";
                        return false;
                    }
                }
                i += 1;
            }
            else
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
            }

            if (bbox[2] <= bbox[0] || bbox[3] <= bbox[1])
            {
                errCode = "INVALID_BBOX";
                errMsg  = "--bbox 窗口无效：要求 xmax > xmin 且 ymax > ymin";
                return false;
            }
            return true;
        }

        bool HasHelpFlag(const std::vector<std::string>& args, size_t startIdx)
        {
            for (size_t i = startIdx; i < args.size(); ++i)
            {
                if (args[i] == "--help" || args[i] == "-h")
                    return true;
            }
            return false;
        }

        CommandKind ParseHelpTopic(const std::string& name)
        {
            if (name == "export-db") return CommandKind::ExportDb;
            if (name == "render")    return CommandKind::Render;
            if (name == "diff")      return CommandKind::Diff;
            return CommandKind::None;
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

    std::string BuildUsageText(CommandKind topic)
    {
        if (topic == CommandKind::ExportDb)
        {
            return
                "SaDwgTool export-db - 将 DWG/DXF 导出为 Schema v1.0 SQLite 库（19 张表 + 2 张 R-tree）\n"
                "\n"
                "用法:\n"
                "  SaDwgTool export-db <in.dwg|dxf> <out.sqlite> [选项]\n"
                "\n"
                "选项:\n"
                "  --reuse-if-fresh      输出库存在且源文件未变动时直接复用缓存（默认关闭）\n"
                "  --force               忽略缓存强制重新导出（默认关闭）\n"
                "  --no-block-entities   跳过导出 block_entities 块定义内部图元表（默认导出）\n"
                "  --fonts-dir <dir>     指定 SHX/TTF 字体目录（默认自动探测）\n"
                "  --pretty              美化缩进输出 JSON（默认紧凑单行）\n"
                "  --verbose             输出详细日志到 stderr（默认仅错误）\n"
                "  -h, --help            显示本帮助信息（退出码 0）\n"
                "\n"
                "示例:\n"
                "  SaDwgTool export-db drawing.dwg cache.sqlite --reuse-if-fresh\n";
        }

        if (topic == CommandKind::Render)
        {
            return
                "SaDwgTool render - 纯 CPU 无头渲染单图或双图叠加 PNG\n"
                "\n"
                "用法:\n"
                "  SaDwgTool render <in.dwg|dxf> <out.png> [选项]\n"
                "\n"
                "选项:\n"
                "  --compare <b.dwg|dxf>           叠加渲染对比图 B（A 青 / B 红 / 重合灰）\n"
                "  --offset <dx> <dy>              叠加对比时 B->A 平移偏移量（默认 0 0）\n"
                "  --layout <name>                 布局名、Model/模型 或 index:<N>（默认 Model）\n"
                "  --layout-b <name>               图 B 布局名（默认自动同名/同序号匹配）\n"
                "  --bbox <xmin> <ymin> <xmax> <ymax>\n"
                "  --bbox <xmin,ymin,xmax,ymax>    裁剪窗口，空格 4 参与逗号单参两种写法均支持（默认整图）\n"
                "  --mm-per-px <f>                 分辨率，单位 mm/px，正数（默认自动按 --max-px 适配）\n"
                "  --max-px <n>                    画布长边像素上限，范围 16..8192（默认 2400）\n"
                "  --bg <white|black>              背景色（默认 white）\n"
                "  --line-width-px <f>             线宽像素，范围 1.0..4.0（默认 1.0）\n"
                "  --include-layers <re>           仅渲染匹配正则的图层（不区分大小写，含块内递归）\n"
                "  --exclude-layers <re>           排除匹配正则的图层（不区分大小写，含块内递归）\n"
                "  --exclude-annotations           排除文字、标注、引线等注记图元（默认包含）\n"
                "  --exclude-frames                自动排除天正修改范围框（默认不排除）\n"
                "  --fonts-dir <dir>               指定 SHX/TTF 字体目录（默认自动探测）\n"
                "  --pretty                        美化缩进输出 JSON（默认紧凑单行）\n"
                "  --verbose                       输出详细日志到 stderr（默认仅错误）\n"
                "  -h, --help                      显示本帮助信息（退出码 0）\n"
                "\n"
                "示例:\n"
                "  SaDwgTool render a.dwg preview.png --layout \"布局1\" --max-px 2400\n"
                "  SaDwgTool render a.dwg crop.png --compare b.dwg --bbox 1000,2000,5000,4500\n";
        }

        if (topic == CommandKind::Diff)
        {
            return
                "SaDwgTool diff - 三轨确定性 DWG/DXF 比对（视觉像素差分 + 文字差分 + 天正语义差分）\n"
                "\n"
                "用法:\n"
                "  SaDwgTool diff <a.dwg|dxf> <b.dwg|dxf> [选项]\n"
                "\n"
                "输出选项:\n"
                "  --out-png <png>                 输出三态差异叠加 PNG（默认不写图片）\n"
                "  --out-json <json>               写入完整 JSON 报告（传 --out-png 时默认写同名 .json）\n"
                "  --out-db <sqlite>               输出 4 表比对 SQLite 库（含 diff_regions_rtree）\n"
                "  --no-region-boxes               PNG 中不绘制橙色差异区域框与编号（默认绘制）\n"
                "\n"
                "范围与对齐选项:\n"
                "  --layout <name>                 布局名、Model/模型 或 index:<N>（默认 Model）\n"
                "  --layout-b <name>               图 B 布局名（默认自动同名/同序号匹配）\n"
                "  --bbox <xmin> <ymin> <xmax> <ymax>\n"
                "  --bbox <xmin,ymin,xmax,ymax>    比对窗口（A 坐标系），空格 4 参与逗号单参均支持（默认整图）\n"
                "  --auto-align                    自动按天正轴网/轴线层计算 B->A 对齐偏移（默认关闭）\n"
                "  --offset <dx> <dy>              手动指定 B->A 平移偏移，优先于 --auto-align（默认 0 0）\n"
                "  --align-layers-a <re>           自动对齐图 A 轴线层正则（默认 DOTE|AXIS|轴线|轴网|GRID）\n"
                "  --align-layers-b <re>           自动对齐图 B 轴线层正则（默认同上）\n"
                "  --align-tol <mm>                自动对齐投票容差（默认 5.0）\n"
                "  --align-min-span <mm>           自动对齐候选轴线最小长度（默认 5000.0）\n"
                "\n"
                "尺度与容差选项（纸张布局非 Model 时未显式指定则自动切为纸张默认尺度）:\n"
                "  --mm-per-px <f>                 光栅分辨率 mm/px（模型默认 20.0，纸张布局自动 1.0）\n"
                "  --max-px <n>                    画布长边像素上限，范围 16..8192（默认 4096）\n"
                "  --tolerance-mm <f>              像素膨胀容差 mm，别名 --tolerance（模型默认 30.0，纸张自动 1.0）\n"
                "  --min-region-mm <f>             差异区最小长边 mm（模型默认 800.0，纸张自动 2.0）\n"
                "  --region-gap-mm <f>             相邻差异合并间距 mm（模型默认 1200.0，纸张自动 8.0）\n"
                "  --max-region-mm <f>             单差异区最大长边，超限递归切分（默认 15000.0）\n"
                "  --text-match-tol-mm <f>         轨 2 文字空间配对容差 mm（默认 500.0）\n"
                "  --tch-match-tol-mm <f>          轨 3 天正构件空间配对容差 mm（默认 300.0）\n"
                "\n"
                "过滤与通用选项:\n"
                "  --include-layers <re>           仅比对匹配正则的图层（不区分大小写）\n"
                "  --exclude-layers <re>           排除匹配正则的图层（不区分大小写）\n"
                "  --include-annotations           轨 1 掩膜包含注记（默认排除注记，轨 2 文字比对不受影响）\n"
                "  --exclude-frames                自动排除天正修改范围框（默认不排除）\n"
                "  --fonts-dir <dir>               指定 SHX/TTF 字体目录（默认自动探测）\n"
                "  --pretty                        美化缩进输出 JSON（默认紧凑单行）\n"
                "  --verbose                       输出详细日志到 stderr（默认仅错误）\n"
                "  -h, --help                      显示本帮助信息（退出码 0）\n"
                "\n"
                "示例:\n"
                "  SaDwgTool diff a.dwg b.dwg --layout \"布局1\" --out-png diff.png --out-json diff.json\n"
                "  SaDwgTool diff a.dwg b.dwg --auto-align --bbox 0,0,20000,15000 --out-png diff.png --out-db diff.sqlite\n";
        }

        return
            "SaDwgTool - 原生 64 位无头 DWG/DXF 解析、渲染与三轨比对引擎\n"
            "\n"
            "用法:\n"
            "  SaDwgTool --version | -v\n"
            "  SaDwgTool --help | -h | help [export-db|render|diff]\n"
            "  SaDwgTool export-db <in.dwg|dxf> <out.sqlite> [--reuse-if-fresh] [--force] [--no-block-entities] [--fonts-dir <dir>] [--pretty] [--verbose]\n"
            "  SaDwgTool render <in.dwg|dxf> <out.png> [--compare <b.dwg|dxf>] [--offset <dx> <dy>] [--layout <name>] [--layout-b <name>]\n"
            "                   [--bbox <xmin> <ymin> <xmax> <ymax> | <xmin,ymin,xmax,ymax>] [--mm-per-px <f>] [--max-px <n>]\n"
            "                   [--bg white|black] [--line-width-px <f>] [--include-layers <re>] [--exclude-layers <re>]\n"
            "                   [--exclude-annotations] [--exclude-frames] [--fonts-dir <dir>] [--pretty] [--verbose]\n"
            "  SaDwgTool diff <a.dwg|dxf> <b.dwg|dxf> [--out-png <png>] [--out-json <json>] [--out-db <sqlite>]\n"
            "                 [--layout <name>] [--layout-b <name>] [--bbox <xmin> <ymin> <xmax> <ymax> | <xmin,ymin,xmax,ymax>]\n"
            "                 [--auto-align] [--offset <dx> <dy>] [--align-layers-a <re>] [--align-layers-b <re>]\n"
            "                 [--align-tol <mm>] [--align-min-span <mm>] [--mm-per-px <f>] [--max-px <n>]\n"
            "                 [--tolerance-mm <f>] [--min-region-mm <f>] [--region-gap-mm <f>] [--max-region-mm <f>]\n"
            "                 [--include-layers <re>] [--exclude-layers <re>] [--include-annotations] [--exclude-frames]\n"
            "                 [--text-match-tol-mm <f>] [--tch-match-tol-mm <f>] [--no-region-boxes] [--fonts-dir <dir>] [--pretty] [--verbose]\n"
            "\n"
            "说明:\n"
            "  - 查看子命令完整参数表与默认值：SaDwgTool <export-db|render|diff> --help\n"
            "  - --bbox 同时支持空格 4 参（--bbox xmin ymin xmax ymax）与逗号单参（--bbox xmin,ymin,xmax,ymax）。\n"
            "  - diff 在纸张布局（--layout 非 Model）下未显式传尺度参数时，自动切换为纸张尺度默认值：\n"
            "    mm_per_px=1.0, tolerance_mm=1.0, min_region_mm=2.0, region_gap_mm=8.0（模型空间默认 20.0 / 30.0 / 800.0 / 1200.0）。\n"
            "  - 退出码：0=成功，1=运行时错误，2=参数错误。\n"
            "\n"
            "常用示例:\n"
            "  SaDwgTool export-db drawing.dwg cache.sqlite --reuse-if-fresh\n"
            "  SaDwgTool render a.dwg crop.png --compare b.dwg --layout \"布局1\" --bbox 100,200,500,450\n"
            "  SaDwgTool diff a.dwg b.dwg --layout \"布局1\" --out-png diff.png --out-json diff.json\n";
    }

    bool ParseCliArgs(const std::vector<std::string>& args, ParsedCommand& out,
                      std::string& errorCode, std::string& errorMsg)
    {
        out = {};
        if (args.empty())
        {
            errorCode = "MISSING_COMMAND";
            errorMsg  = "缺少子命令（支持 export-db / render / diff / --version / --help）";
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
            if (args.size() >= 2)
                out.HelpTopic = ParseHelpTopic(args[1]);
            return true;
        }

        if (cmd == "export-db")
        {
            if (HasHelpFlag(args, 1))
            {
                out.Kind      = CommandKind::Help;
                out.HelpTopic = CommandKind::ExportDb;
                return true;
            }
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
            if (HasHelpFlag(args, 1))
            {
                out.Kind      = CommandKind::Help;
                out.HelpTopic = CommandKind::Render;
                return true;
            }
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
                    if (!ParseBbox(args, i, out.Render.Bbox, errorCode, errorMsg)) return false;
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
            if (HasHelpFlag(args, 1))
            {
                out.Kind      = CommandKind::Help;
                out.HelpTopic = CommandKind::Diff;
                return true;
            }
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
                    if (!ParseBbox(args, i, out.Diff.Bbox, errorCode, errorMsg)) return false;
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
                    out.Diff.MmPerPxExplicit = true;
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
                    out.Diff.ToleranceMmExplicit = true;
                }
                else if (a == "--min-region-mm")
                {
                    if (!NeedArgs(args, i, 1, a, errorCode, errorMsg)) return false;
                    if (!ParseDoubleStrict(args[++i], out.Diff.MinRegionMm) || out.Diff.MinRegionMm < 0.0)
                    {
                        errorCode = "INVALID_ARGS"; errorMsg = "--min-region-mm 不能为负数"; return false;
                    }
                    out.Diff.MinRegionMmExplicit = true;
                }
                else if (a == "--region-gap-mm")
                {
                    if (!NeedArgs(args, i, 1, a, errorCode, errorMsg)) return false;
                    if (!ParseDoubleStrict(args[++i], out.Diff.RegionGapMm) || out.Diff.RegionGapMm <= 0.0)
                    {
                        errorCode = "INVALID_ARGS"; errorMsg = "--region-gap-mm 必须为正数"; return false;
                    }
                    out.Diff.RegionGapMmExplicit = true;
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
        errorMsg  = "未知子命令：" + cmd + "（支持 export-db / render / diff / --version / --help）";
        return false;
    }
}
