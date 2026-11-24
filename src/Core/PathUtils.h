#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace MiniCAD
{
    // UTF-8 字符串 → std::filesystem::path。
    //
    // 为什么必须有这个转换：Windows 上 std::filesystem::path 的窄字符构造函数按
    // 「当前 ANSI 代码页」（简体中文机器上是 GBK）解释字节，把 UTF-8 路径直接传进去会得到
    // 乱码路径——文件明明存在却打不开。
    // 直接 `std::ifstream(std::string)` 更糟：同样按 ANSI 解释，且没有任何补救机会。
    //
    // 走 char8_t 构造则明确声明"这串字节是 UTF-8"，由标准库转到本机宽字符表示，
    // 因此含中文、日文等非 ASCII 的路径也能正确打开（Windows 与 POSIX 都适用）。
    //
    // 项目里凡是"来自界面 / 数据文件的路径"都应经这里转换后再访问文件系统。
    inline std::filesystem::path Utf8ToPath(const std::string& utf8)
    {
        return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
    }

    // 统一采集普通文件的字节大小与毫秒级修改时间戳（用于导入窗口双 stat 校验与 SQLite 缓存指纹）。
    // 任一步失败均返回 false 并清零输出。
    inline bool TryStatFileVersion(const std::filesystem::path& path,
                                   std::uint64_t& sizeBytes,
                                   std::int64_t& mtimeMs)
    {
        sizeBytes = 0;
        mtimeMs   = 0;
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || ec)
            return false;
        if (!std::filesystem::is_regular_file(path, ec) || ec)
            return false;
        const auto sz = std::filesystem::file_size(path, ec);
        if (ec)
            return false;
        const auto ftime = std::filesystem::last_write_time(path, ec);
        if (ec)
            return false;
#if defined(_LIBCPP_VERSION)
        mtimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(ftime.time_since_epoch()).count();
#else
        const auto sysTime = std::chrono::clock_cast<std::chrono::system_clock>(ftime);
        mtimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(sysTime.time_since_epoch()).count();
#endif
        sizeBytes = static_cast<std::uint64_t>(sz);
        return true;
    }
}
