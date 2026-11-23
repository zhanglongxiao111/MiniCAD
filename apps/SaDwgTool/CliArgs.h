#pragma once
// SaDwgTool 命令行参数结构与严格解析器（遵循设计方案 §5 契约）。
#include <array>
#include <optional>
#include <string>
#include <vector>

namespace MiniCAD::SaTool
{
    enum class CommandKind
    {
        None,
        Version,
        Help,
        ExportDb,
        Render,
        Diff,
    };

    struct ExportDbOptions
    {
        std::string InputPath;
        std::string OutputDbPath;
        bool        ReuseIfFresh    = false;
        bool        Force           = false;
        bool        NoBlockEntities = false;
        std::string FontsDir;
        bool        Pretty          = false;
        bool        Verbose         = false;
    };

    struct RenderOptions
    {
        std::string InputPath;
        std::string OutputPngPath;
        std::string ComparePath;
        double      OffsetDx = 0.0;
        double      OffsetDy = 0.0;
        std::string LayoutName = "Model";
        std::string LayoutNameB;          // 空 = 自动匹配
        bool        HasBbox = false;
        std::array<double, 4> Bbox{};     // xmin, ymin, xmax, ymax
        double      MmPerPx = 0.0;        // <= 0 表示自动
        int         MaxPx   = 2400;       // 硬顶 8192
        bool        DarkBg  = false;      // 默认 white（false）
        float       LineWidthPx = 1.0f;   // 1.0..4.0
        std::string IncludeLayersRegex;
        std::string ExcludeLayersRegex;
        bool        ExcludeAnnotations = false;
        bool        ExcludeFrames      = false;
        std::string FontsDir;
        bool        Pretty  = false;
        bool        Verbose = false;
    };

    struct DiffOptions
    {
        std::string InputPathA;
        std::string InputPathB;
        std::string OutPngPath;
        std::string OutJsonPath;
        bool        OutJsonExplicit = false;
        std::string OutDbPath;
        std::string LayoutName  = "Model";
        std::string LayoutNameB;          // 空 = 自动匹配
        bool        HasBbox = false;
        std::array<double, 4> Bbox{};
        bool        AutoAlign    = false;
        bool        HasOffset    = false;
        double      OffsetDx     = 0.0;
        double      OffsetDy     = 0.0;
        std::string AlignLayersA;
        std::string AlignLayersB;
        double      AlignTolMm      = 5.0;
        double      AlignMinSpanMm  = 5000.0;
        double      MmPerPx             = 20.0;
        bool        MmPerPxExplicit     = false;
        int         MaxPx               = 4096;
        double      ToleranceMm         = 30.0;
        bool        ToleranceMmExplicit = false;
        double      MinRegionMm         = 800.0;
        bool        MinRegionMmExplicit = false;
        double      RegionGapMm         = 1200.0;
        bool        RegionGapMmExplicit = false;
        double      MaxRegionMm         = 15000.0;
        std::string IncludeLayersRegex;
        std::string ExcludeLayersRegex;
        bool        IncludeAnnotations = false;
        bool        ExcludeFrames      = false;
        double      TextMatchTolMm     = 500.0;
        double      TchMatchTolMm      = 300.0;
        bool        NoRegionBoxes      = false;
        std::string FontsDir;
        bool        Pretty  = false;
        bool        Verbose = false;
    };

    struct ParsedCommand
    {
        CommandKind     Kind      = CommandKind::None;
        CommandKind     HelpTopic = CommandKind::None; // None = 总览，否则对应子命令详细帮助
        ExportDbOptions ExportDb;
        RenderOptions   Render;
        DiffOptions     Diff;
        bool            Verbose = false;
        bool            Pretty  = false;
    };

    // 解析 UTF-8 参数列表（不含 argv[0]）；失败时填充 errorCode 与 errorMsg 并返回 false（退出码 2）。
    bool ParseCliArgs(const std::vector<std::string>& args, ParsedCommand& out,
                      std::string& errorCode, std::string& errorMsg);

    std::string BuildUsageText(CommandKind topic = CommandKind::None);
}
