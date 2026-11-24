#pragma once
// 输入/输出文件路径身份冲突守卫（CAD-22）：
// - 所有子命令在首次写盘前统一检查：
//   1. 任一输出路径（显式或自动派生）不得与任一输入路径指向同一文件（杜绝覆盖原 DWG）；
//   2. 多输出路径之间不得互相冲突，且输出路径不得指向已存在的目录。
// - 已存在文件使用 Windows GetFileInformationByHandle（及 std::filesystem::equivalent）按真实文件身份判等，
//   覆盖大小写、8.3 短名、相对路径、斜杠混用、符号链接与硬链接；
// - 尚未创建的新路径按“最长已存在祖先真实路径 + 词法归一化后缀 + 大小写归一化”判等。
#include <string>
#include <vector>

namespace MiniCAD::SaTool
{
    struct PathGuardEntry
    {
        std::string Label;
        std::string Path;
    };

    std::string DeriveDefaultJsonPathForPng(const std::string& pngPath);

    bool PathsReferToSameFile(const std::string& utf8A, const std::string& utf8B);

    bool ValidateIoPathConflicts(const std::vector<PathGuardEntry>& inputs,
                                 const std::vector<PathGuardEntry>& outputs,
                                 std::string* errorCode = nullptr,
                                 std::string* errorMsg = nullptr);
}
