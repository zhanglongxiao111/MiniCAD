#pragma once
// 天正 8 张原生语义表写入器 + 天正可读文字提取器（与 SqliteExporter / DiffEngine 共用）。
#include "Import/CadSemanticSnapshot.h"
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

struct sqlite3;

namespace MiniCAD::SaTool
{
    struct ExtractedText
    {
        int64_t     EntityId    = 0;      // 0 表示 NULL
        std::string OwnerHandle;          // 大写十六进制，空表示 NULL
        std::string SourceKind;           // TEXT | MTEXT | ATTRIB | DIMENSION | MLEADER | TABLE | TCH_*
        std::string Content;              // 去格式码后的纯 UTF-8 文本
        std::string RawContent;           // 与 Content 相同则留空（导出 NULL）
        std::string Tag;                  // 非 ATTRIB 留空（导出 NULL）
        std::string Layer;
        std::string Space;                // "model" | "paper"
        std::string LayoutName;
        std::string BlockPath;
        double      X           = 0.0;
        double      Y           = 0.0;
        double      Height      = 0.0;
        double      RotationDeg = 0.0;
        double      MinX        = 0.0;
        double      MinY        = 0.0;
        double      MaxX        = 0.0;
        double      MaxY        = 0.0;
    };

    struct TchExportCounts
    {
        int Walls      = 0;
        int Openings   = 0;
        int Columns    = 0;
        int Spaces     = 0;
        int Stairs     = 0;
        int Dimensions = 0;
        int Axes       = 0;
        int Sheets     = 0;
    };

    // 将 CadSemanticSnapshot 中的天正原生语义写入 8 张 tch_* 表（调用方已开启事务与建表）。
    bool WriteTchSemanticTables(sqlite3* db, const CadSemanticSnapshot& snap,
                                TchExportCounts& counts, std::string* error = nullptr);

    // 收集天正原生解码对象中的可读文字（TCH_TEXT/MTEXT/ELEVATION/SYMBOL、门窗编号、轴号、房间名、标注值、表格单元格），
    // 同时把已覆盖的天正对象句柄写入 emittedHandles，供主文字遍历器避免重复扫描 *U_TCH_* 内部块。
    void CollectTchReadableTexts(const CadSemanticSnapshot& snap,
                                 std::vector<ExtractedText>& out,
                                 std::unordered_set<uint64_t>& emittedHandles);
}
