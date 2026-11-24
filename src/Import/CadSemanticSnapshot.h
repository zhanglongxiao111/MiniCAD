#pragma once
// DWG/DXF 导入期间单趟顺带采集的原图元数据与天正原生语义快照（零 MiniDWG 公开类型依赖）。
// 仅在调用方显式传入 CadSemanticSnapshot* 时启用；默认 nullptr 零额外内存与计算开销。
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace MiniCAD
{
    struct CadSemanticSnapshot
    {
        // ── 文件与头信息 ────────────────────────────────────────────────
        std::string Format;              // "DWG" | "DXF"
        std::string DwgVersion;          // "AC1015" .. "AC1032"
        std::string CodePage;            // 如 "ANSI_936"
        int         InsunitsCode = 0;    // DXF $INSUNITS（4 = mm, 6 = m 等）
        std::string InsunitsName = "unitless";
        std::string Measurement  = "metric"; // "metric" | "imperial"
        double      LtScale      = 1.0;
        bool        PsLtScale    = true;
        bool        TchConfigFill = false;
        double      TchConfigDetailScale = 100.0;

        int TchObjectCount  = 0;         // 原图中模型/图纸空间的 TCH_* 对象总数
        int TchDecodedCount = 0;         // 原生解码成功的 TCH 对象总数
        int TchFailedCount  = 0;         // 解码失败的 TCH 对象总数

        // ── 导入诊断明细（未解码代理、跳过实体、天正失败等）──────────────
        struct DiagnosticItem
        {
            std::string Kind;            // "tch_failed" | "undecoded_proxy" | "skipped_entity" | "unresolved_xref"
            std::string DxfName;
            std::string Reason;
            int         Count = 1;
            uint64_t    SampleHandle = 0;
        };
        std::vector<DiagnosticItem> Diagnostics;

        // ── 外部参照加载事实（导入器单趟记录，供 SQLite 缓存指纹直接消费）──
        enum class XrefLoadStatus
        {
            Loaded,      // 已成功定位、读盘并导入宿主块
            Missing,     // 磁盘上未能定位到目标外参文件
            LoadFailed,  // 已定位文件（或无宿主路径/循环引用/超深度），但未成功读入
        };
        struct XrefLoadRecord
        {
            std::string    BlockName;
            std::string    ParentDir;      // 本次解析使用的宿主/父外参所在目录（UTF-8）
            std::string    RawPath;        // 图纸中保存的原始外参路径
            std::string    ResolvedPath;   // 定位到的规范文件路径（UTF-8；Missing 时为空）
            XrefLoadStatus Status          = XrefLoadStatus::Missing;
            bool           VersionVerified = false; // 读取窗口前后双 stat 与实际读出字节数完全一致
            uint64_t       SizeBytes       = 0;
            int64_t        MtimeMs         = 0;
        };
        std::vector<XrefLoadRecord> XrefLoads;

        // ── 图层原始分离标志（保留 is_on / is_frozen 原始语义）──────────
        struct LayerRawFlags
        {
            uint64_t    Handle       = 0;
            std::string Name;
            bool        HasTrueColor = false;
            int         ColorAci     = 7;   // 真彩色时不使用（导出 NULL）
            uint32_t    TrueColorRgb = 0;
            bool        IsOn         = true;
            bool        IsFrozen     = false;
            bool        IsLocked     = false;
            bool        IsPlottable  = true;
        };
        std::unordered_map<uint32_t, LayerRawFlags> LayerFlagsById;

        // ── Scene ObjectID → 原图句柄与原始 DXF 类名 ────────────────────
        struct EntityOrigin
        {
            uint64_t    Handle           = 0;
            std::string DxfName;
            bool        IsTchSynthesized = false; // 该 Scene 实体由 TCH_* 生成（内部可能包了 *U_TCH_* 块）
        };
        std::unordered_map<uint64_t, EntityOrigin> EntityOrigins;

        // ── 天正 1：墙 / 幕墙（对齐 TchWall.h）──────────────────────────
        struct TchWallRecord
        {
            uint64_t    Handle = 0;
            std::string DxfName;                  // "TCH_WALL" | "TCH_CURTAIN_WALL"
            std::string Layer;
            std::string Space;                    // "model" | "paper"
            std::string LayoutName;
            std::string BlockName;                // 空 = 顶层模型/图纸空间
            double      StartX = 0, StartY = 0, StartZ = 0;
            double      EndX   = 0, EndY   = 0, EndZ   = 0;
            double      Length = 0;
            double      ArcAngleRad = 0;
            bool        HasArc = false;
            double      ArcCx = 0, ArcCy = 0, ArcRadius = 0;
            double      LeftWidth = 0, RightWidth = 0, TotalThickness = 0;
            bool        HasHeight = false;
            double      Height = 0;
            int         MaterialCode = 0;
            std::string MaterialName;
            uint32_t    Flags = 0;
            double      Scale = 100;
            int         OpeningCount = 0;
            std::array<double, 4>   Joins{};
            bool                    HasConnections = false;
            std::array<uint64_t, 4> ConnectedHandles{};
            double      MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
        };
        std::vector<TchWallRecord> TchWalls;

        // ── 天正 2：门 / 窗 / 洞口 / 带形窗 / 组合门窗（对齐 TchOpening.h）──
        struct TchOpeningRecord
        {
            uint64_t    Handle       = 0;
            uint64_t    ParentHandle = 0;         // 组合门窗子项指向父门窗句柄，顶层为 0
            std::string Layer;
            std::string LabelLayer;
            std::string Space;
            std::string LayoutName;
            std::string BlockName;
            int         KindCode = 0;             // 0 Door, 1 Window, 2 Hole, 5 Band, 8 Group
            std::string KindName;                 // 门 / 窗 / 洞口 / 带形窗 / 组合门窗
            std::string Label;
            double      X = 0, Y = 0, SillZ = 0;
            double      Width = 0, Height = 0, Thickness = 0;
            double      AngleRad = 0;
            int         Mirror = 0;
            bool        MirrorX = false, MirrorY = false;
            uint64_t    WallHandle = 0;
            double      LabelX = 0, LabelY = 0, LabelHeight = 3.5;
            double      Scale = 100;
            int         Flags = 0;
            int32_t     DisplayFlags = 0;
            double      MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
        };
        std::vector<TchOpeningRecord> TchOpenings;

        // ── 天正 3：柱（对齐 TchColumn.h）───────────────────────────────
        struct TchColumnVertex
        {
            double X = 0, Y = 0, Bulge = 0;
        };
        struct TchColumnRecord
        {
            uint64_t    Handle = 0;
            std::string Layer;
            std::string Space;
            std::string LayoutName;
            std::string BlockName;
            double      X = 0, Y = 0, BaseZ = 0;
            double      Height = 0;
            int         MaterialCode = 0;
            std::string MaterialName;
            double      Scale = 100;
            bool        HasArea = false;
            double      AreaM2 = 0;
            std::vector<TchColumnVertex> Outline;
            double      MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
        };
        std::vector<TchColumnRecord> TchColumns;

        // ── 天正 4：房间面积（对齐 TchSpace.h）──────────────────────────
        struct TchSpaceRecord
        {
            uint64_t    Handle = 0;
            std::string Layer;
            std::string Space;
            std::string LayoutName;
            std::string BlockName;
            std::string Name;
            double      AreaM2 = 0;
            double      PerimeterMm = 0;
            double      CentroidX = 0, CentroidY = 0;
            double      Scale = 1.0;
            std::vector<std::pair<double, double>> Outline;
            double      MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
        };
        std::vector<TchSpaceRecord> TchSpaces;

        // ── 天正 5：楼梯（对齐 TchStair.h + TchMultiStair.h）────────────
        struct TchStairRecord
        {
            uint64_t    Handle = 0;
            std::string StairKind;                // "TCH_RECTSTAIR" 等
            std::string Layer;
            std::string Space;
            std::string LayoutName;
            std::string BlockName;
            double      X = 0, Y = 0, RotationDeg = 0;
            std::optional<double> StepHeight;
            std::optional<double> TreadWidth;
            std::optional<double> FlightWidth;
            std::optional<double> LandingWidth;
            std::optional<double> TotalWidth;
            std::optional<int>    Steps1;
            std::optional<int>    Steps2;
            std::optional<int>    FloorType;      // 0 首层 / 1 中间层 / 2 顶层
            std::string           FloorTypeName;
            std::optional<bool>   Mirror;
            std::optional<double> Scale;
            std::string           ExtraJson;
            double      MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
        };
        std::vector<TchStairRecord> TchStairs;

        // ── 天正 6：尺寸 / 半径标注（对齐 TchDimension.h）───────────────
        struct TchDimensionRecord
        {
            uint64_t    Handle = 0;
            std::string DxfName;                  // "TCH_DIMENSION2" | "TCH_DIMENSION" | "TCH_RADIUSDIM"
            std::string Layer;
            std::string Space;
            std::string LayoutName;
            std::string BlockName;
            double      X = 0, Y = 0, Z = 0;
            double      AngleDeg = 0;
            double      OffsetVal = 0;
            bool        IsArc = false;
            std::optional<double> ArcRadius;
            std::optional<double> MeasuredRadius;
            std::vector<double>        Spans;
            double                     TotalSpan = 0;
            std::map<int, std::string> TextOverrides;
            int         Flags = 0;
            double      Scale = 1.0;
            double      MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
        };
        std::vector<TchDimensionRecord> TchDimensions;

        // ── 天正 7：轴网轴线展开行（对齐 TchDimension.h::BuildAxisLabel）──
        struct TchAxisRecord
        {
            uint64_t    GroupHandle = 0;
            int         AxisIndex   = 0;
            std::string Layer;
            std::string TextLayer;
            std::string Space;
            std::string LayoutName;
            std::string BlockName;
            std::string Label;
            double      Spacing         = 0;
            double      OffsetFromFirst = 0;
            double      X1 = 0, Y1 = 0, X2 = 0, Y2 = 0;
            std::string Orientation;              // "vertical" | "horizontal" | "oblique"
            std::optional<double> Coord;          // vertical 取 x1，horizontal 取 y1，oblique 空
            double      Scale    = 1.0;
            bool        ShowText = true;
            int         Flags    = 0;
            double      MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
        };
        std::vector<TchAxisRecord> TchAxes;

        // ── 天正 8：表格（对齐 TchSheet.h）──────────────────────────────
        struct TchSheetRecord
        {
            uint64_t    Handle = 0;
            std::string Layer;
            std::string Space;
            std::string LayoutName;
            std::string BlockName;
            std::string Title;
            int         RowCount = 0;
            int         ColCount = 0;
            double      X = 0, Y = 0, RotationDeg = 0;
            std::vector<std::vector<std::string>> Cells;
            double      MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
        };
        std::vector<TchSheetRecord> TchSheets;

        // ── 天正可读文字（用于全量进 texts 表且零重复）───────────────────
        struct TchTextRecord
        {
            uint64_t    OwnerHandle = 0;
            std::string SourceKind;               // "TCH_TEXT" | "TCH_MTEXT" | "TCH_ELEVATION" | "TCH_SYMBOL" 等
            std::string Layer;
            std::string Space;
            std::string LayoutName;
            std::string BlockName;
            double      X = 0, Y = 0;
            double      Height = 0;
            double      RotationDeg = 0;
            std::string Content;
        };
        std::vector<TchTextRecord> TchTexts;
    };
}
