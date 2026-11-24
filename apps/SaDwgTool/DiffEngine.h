#pragma once
// 三轨确定性 DWG/DXF 比对引擎：
//   - 自动对齐（优先 tch_axes，次选轴线层 1D 共识投票）
//   - 轨 1：可分离 O(W*H) 膨胀容差差分 + 粗网格 8 连通 BFS 聚类 + 递归中位数切分 + 细子框
//   - 轨 2：独立空间文字与属性比对（两阶段：精确文本优先 -> 同位改字）
//   - 轨 3：原生天正构件语义比对（墙/门窗/柱/房间/楼梯两级匹配与参数差分）
//   - 输出三态叠加 PNG、完整 JSON 报告、可选 4 表 diff.sqlite（支持 ATTACH 联表分析）
#include "CliArgs.h"
#include "Import/CadSemanticSnapshot.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace MiniCAD::SaTool
{
    struct AlignCandidate
    {
        double Delta = 0.0;
        int    Votes = 0;
    };

    struct AlignmentInfo
    {
        std::string                 Mode       = "none";    // "auto" | "manual" | "none"
        std::string                 Source     = "none";    // "tch_axes" | "layer_lines" | "manual" | "none"
        std::string                 Confidence = "none";    // "high" | "low" | "failed" | "manual" | "none"
        double                      Dx         = 0.0;
        double                      Dy         = 0.0;
        int                         SupportX   = 0;
        int                         SupportY   = 0;
        std::vector<AlignCandidate> CandidatesX;
        std::vector<AlignCandidate> CandidatesY;
    };

    struct DiffSideSummary
    {
        std::string Source;
        std::string Layout;
        int         EntitiesRendered = 0;
        int         EntitiesFiltered = 0;
        uint64_t    InkPixels        = 0;
        bool        IsNativeTianzheng = false;
        int         UndecodedProxies = 0;
        int         UnresolvedXrefs  = 0;
    };

    struct DiffRegionItem
    {
        int                                Id = 0;
        std::array<double, 4>              BboxDrawing{};    // [xmin, ymin, xmax, ymax] in A space
        std::array<int, 4>                 BboxPx{};         // [x0, y0, x1, y1]
        int64_t                            AOnlyPixels = 0;
        int64_t                            BOnlyPixels = 0;
        std::string                        DominantSide;     // "a_only" | "b_only" | "both"
        std::vector<std::array<double, 4>> SubBoxesDrawing;
    };

    struct DiffTextItem
    {
        std::string ChangeType;        // "a_only" | "b_only" | "modified"
        std::string SourceKind;
        std::string Tag;
        double      X = 0.0;
        double      Y = 0.0;
        std::string AText;
        std::string BText;
        std::string ALayer;
        std::string BLayer;
        std::string AOwnerHandle;
        std::string BOwnerHandle;
        bool        HasDistMm = false;
        double      DistMm    = 0.0;
        int         RegionId  = 0;     // 0 表示 NULL
    };

    struct DiffTchChangeItem
    {
        std::string           Category;     // "wall" | "opening" | "column" | "space" | "stair"
        std::string           ChangeType;   // "a_only" | "b_only" | "modified"
        std::string           HandleA;
        std::string           HandleB;
        std::string           Label;
        double                X = 0.0;
        double                Y = 0.0;
        std::array<double, 4> BboxDrawing{};
        std::string           Summary;
        std::string           BeforeJson;   // 空表示 NULL
        std::string           AfterJson;    // 空表示 NULL
        int                   RegionId = 0; // 0 表示 NULL
    };

    struct DiffReport
    {
        std::string           SchemaVersion = "1.0";
        std::array<double, 4> CropInASpace{};
        std::array<double, 2> OffsetBToA{ 0.0, 0.0 };
        AlignmentInfo         Alignment;
        double                MmPerPx              = 20.0;
        bool                  Clamped              = false;
        double                ToleranceMm          = 30.0;
        double                EffectiveToleranceMm = 30.0;
        int                   TolerancePx          = 1;
        int                   WidthPx              = 0;
        int                   HeightPx             = 0;
        bool                  AnnotationsIncluded  = false;
        std::string           IncludeLayers;
        std::string           ExtraExcludeLayers;
        bool                  ExcludeFrames        = false;
        int                   FramesExcludedA      = 0;
        int                   FramesExcludedB      = 0;
        std::string           FontsDir;
        bool                  FontsFallback        = false;
        bool                  IsPaperSpaceLayout   = false;
        int                   PaperViewportCountA  = 0;
        int                   PaperViewportCountB  = 0;
        bool                  PaperViewportSemanticsExcluded = false;

        DiffSideSummary       SideA;
        DiffSideSummary       SideB;

        // 轨 1 统计
        uint64_t              AOnlyPixels    = 0;
        uint64_t              BOnlyPixels    = 0;
        uint64_t              OverlapPixels  = 0;
        uint64_t              InkUnionPixels = 0;
        double                AOnlyRatio     = 0.0;
        double                BOnlyRatio     = 0.0;
        double                InkBalanceRatio = 1.0;

        double                MinRegionMm = 800.0;
        double                RegionGapMm = 1200.0;
        double                MaxRegionMm = 15000.0;
        std::vector<DiffRegionItem> Regions;

        // 轨 2 统计
        double                    TextMatchTolMm = 500.0;
        int                       TextUnchangedCount = 0;
        int                       TextAOnlyCount     = 0;
        int                       TextBOnlyCount     = 0;
        int                       TextModifiedCount  = 0;
        std::vector<DiffTextItem> TextChanges;

        // 轨 3 统计
        bool                           TchAvailable = false;
        std::string                    TchReason;
        double                         TchMatchTolMm = 300.0;
        int                            TchWallCount    = 0;
        int                            TchOpeningCount = 0;
        int                            TchColumnCount  = 0;
        int                            TchSpaceCount   = 0;
        int                            TchStairCount   = 0;
        std::vector<DiffTchChangeItem> TchChanges;

        // 输出路径
        std::string              PngPath;
        std::string              JsonPath;
        std::string              DbPath;
        std::vector<std::string> Warnings;
        std::string              CoverageNote;
        int64_t                  ElapsedMs = 0;
    };

    // 完整执行 diff 子命令。
    bool RunDiffCommand(const DiffOptions& opts,
                        DiffReport& report,
                        std::string* errorCode = nullptr,
                        std::string* errorMsg = nullptr,
                        bool* isUsageError = nullptr);

    // 原子写出 4 表比对 SQLite 库（含 diff_regions_rtree；全链路错误检查 + RAII 回滚清理）。
    bool WriteDiffSqlite(const std::string& outDbPath, const DiffReport& report, std::string* error = nullptr);

    // 执行轨 3 原生天正构件语义比对（供 RunDiffCommand 调用及单元测试覆盖缺失字段/单侧空场景）。
    void RunTrack3TchDiff(const CadSemanticSnapshot& snapA, const std::string& spaceA, const std::string& layoutA,
                          const CadSemanticSnapshot& snapB, const std::string& spaceB, const std::string& layoutB,
                          const std::array<double, 4>& cropInA,
                          double dx, double dy,
                          double tolMm,
                          const std::vector<DiffRegionItem>& regions,
                          DiffReport& report);

    // 格式化 diff JSON：compactSummary=true 时省略超长明细分项数组（当已写入 --out-json 时给 stdout 用）。
    std::string FormatDiffReportJson(const DiffReport& report, bool compactSummary = false, bool pretty = false);
}
