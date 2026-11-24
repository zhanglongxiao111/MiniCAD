#pragma once
// 将 MiniCAD::Scene + CadSemanticSnapshot 原子导出为 Schema v1.0 标准 SQLite（含 R-tree 空间索引）。
#include "CliArgs.h"
#include "Import/CadSemanticSnapshot.h"
#include "Scene/Scene.h"
#include "TchSemanticExport.h"
#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace MiniCAD::SaTool
{
    struct ExportDbCounts
    {
        int Layers          = 0;
        int Layouts         = 0;
        int Viewports       = 0;
        int Blocks          = 0;
        int BlockEntities   = 0;
        int Inserts         = 0;
        int InsertAttribs   = 0;
        int Texts           = 0;
        int Entities        = 0;
        int TchWalls        = 0;
        int TchOpenings     = 0;
        int TchColumns      = 0;
        int TchSpaces       = 0;
        int TchStairs       = 0;
        int TchDimensions   = 0;
        int TchAxes         = 0;
        int TchSheets       = 0;
        int TchFailed       = 0;
        int UnresolvedXrefs = 0;
    };

    struct ExportDbResult
    {
        bool                  Reused = false;
        std::string           SchemaVersion = "1.0";
        std::string           SourcePath;
        std::string           DbPath;
        std::string           Format;
        std::string           DwgVersion;
        int                   InsunitsCode = 0;
        std::string           InsunitsName;
        bool                  HasModelBbox = false;
        std::array<double, 4> ModelBbox{};
        bool                  IsNativeTianzheng = false;
        ExportDbCounts        Counts;
        int64_t               ElapsedMs = 0;
    };

    // 单一真相源：收集全图所有肉眼可见文字（顶层 + 递归块/MINSERT + DIMENSION/MLEADER/TABLE + 天正原生文字），零重复。
    void CollectAllSceneTexts(const Scene& scene, const CadSemanticSnapshot& snap,
                              std::vector<ExtractedText>& out,
                              const std::unordered_map<uint64_t, int64_t>* objectToEntityId = nullptr);

    // 导出已加载的 Scene + Snapshot 到目标 SQLite 文件（原子写入，无 wal/shm 残留）。
    bool ExportSceneToSqlite(const Scene& scene, const CadSemanticSnapshot& snap,
                             const std::string& sourcePath, uint64_t sourceSizeBytes, int64_t sourceMtimeMs,
                             const std::string& outDbPath, bool includeBlockEntities,
                             ExportDbResult& result, std::string* error = nullptr);

    // 完整执行 export-db 子命令（含路径冲突守卫、--reuse-if-fresh 缓存命中判定与单趟 DWG 解析）。
    bool RunExportDbCommand(const ExportDbOptions& opts, ExportDbResult& result,
                            std::string* error = nullptr,
                            std::string* errorCode = nullptr,
                            bool* isUsageError = nullptr);

    // 格式化 export-db 返回 JSON。
    std::string FormatExportDbResultJson(const ExportDbResult& result, bool pretty = false);

    // 工具：原子移动临时文件到目标路径（UTF-8，支持中文与 UNC 路径）。
    bool AtomicReplaceFile(const std::string& tempUtf8Path, const std::string& destUtf8Path, std::string* error = nullptr);
}
