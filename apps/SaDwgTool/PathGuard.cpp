#include "PathGuard.h"
#include "Core/PathUtils.h"
#include <filesystem>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace MiniCAD::SaTool
{
    namespace fs = std::filesystem;

    std::string DeriveDefaultJsonPathForPng(const std::string& pngPath)
    {
        if (pngPath.empty())
            return {};
        const size_t dot   = pngPath.find_last_of('.');
        const size_t slash = pngPath.find_last_of("/\\");
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
            return pngPath.substr(0, dot) + ".json";
        return pngPath + ".json";
    }

    namespace
    {
#ifdef _WIN32
        struct WinFileIdentity
        {
            DWORD VolumeSerial = 0;
            DWORD IndexHigh    = 0;
            DWORD IndexLow     = 0;
            bool  Valid        = false;

            bool operator==(const WinFileIdentity& o) const
            {
                return Valid && o.Valid
                    && VolumeSerial == o.VolumeSerial
                    && IndexHigh == o.IndexHigh
                    && IndexLow == o.IndexLow;
            }
        };

        WinFileIdentity QueryWinFileIdentity(const fs::path& p)
        {
            WinFileIdentity id;
            HANDLE h = CreateFileW(
                p.c_str(),
                0,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS,
                nullptr);
            if (h == INVALID_HANDLE_VALUE)
                return id;
            BY_HANDLE_FILE_INFORMATION info{};
            if (GetFileInformationByHandle(h, &info))
            {
                id.VolumeSerial = info.dwVolumeSerialNumber;
                id.IndexHigh    = info.nFileIndexHigh;
                id.IndexLow     = info.nFileIndexLow;
                id.Valid        = true;
            }
            CloseHandle(h);
            return id;
        }

        std::wstring QueryFinalPathWin(const fs::path& existingPath)
        {
            HANDLE h = CreateFileW(
                existingPath.c_str(),
                0,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS,
                nullptr);
            if (h == INVALID_HANDLE_VALUE)
                return {};
            const DWORD needed = GetFinalPathNameByHandleW(h, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            if (needed == 0)
            {
                CloseHandle(h);
                return {};
            }
            std::wstring buf(static_cast<size_t>(needed), L'\0');
            const DWORD got = GetFinalPathNameByHandleW(h, buf.data(), needed, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            CloseHandle(h);
            if (got == 0 || got >= needed)
                return {};
            buf.resize(got);
            return buf;
        }

        std::wstring StripWinExtendedPrefix(std::wstring w)
        {
            if (w.rfind(L"\\\\?\\UNC\\", 0) == 0)
                return L"\\\\" + w.substr(8);
            if (w.rfind(L"\\\\?\\", 0) == 0)
                return w.substr(4);
            return w;
        }

        std::wstring NormalizeWinCompareKey(const fs::path& input)
        {
            std::error_code ec;
            fs::path absP = fs::absolute(input, ec);
            if (ec)
                absP = input;
            absP = absP.lexically_normal();

            // 沿目录树向上定位最长已存在祖先，用 GetFinalPathNameByHandleW 消解 8.3 短名、大小写、联接点与符号链接
            fs::path cur = absP;
            std::vector<fs::path> tail;
            while (!cur.empty())
            {
                ec.clear();
                if (fs::exists(cur, ec) && !ec)
                    break;
                const fs::path parent = cur.parent_path();
                if (parent == cur)
                    break;
                if (cur.has_filename())
                    tail.push_back(cur.filename());
                cur = parent;
            }

            fs::path resolved = cur;
            ec.clear();
            if (!cur.empty() && fs::exists(cur, ec) && !ec)
            {
                const std::wstring finalW = QueryFinalPathWin(cur);
                if (!finalW.empty())
                {
                    resolved = fs::path(StripWinExtendedPrefix(finalW));
                }
                else
                {
                    const fs::path canon = fs::weakly_canonical(cur, ec);
                    if (!ec && !canon.empty())
                        resolved = canon;
                }
            }

            for (auto it = tail.rbegin(); it != tail.rend(); ++it)
                resolved /= *it;

            std::wstring w = StripWinExtendedPrefix(resolved.lexically_normal().wstring());
            for (wchar_t& ch : w)
                if (ch == L'/') ch = L'\\';

            // 保留根路径尾斜杠（如 C:\），其余去掉末尾多余反斜杠
            while (w.size() > 3 && w.back() == L'\\')
                w.pop_back();
            if (w.size() == 2 && w[1] == L'\\' && w[0] != L'\\')
                w.pop_back();

            if (!w.empty())
                CharLowerBuffW(w.data(), static_cast<DWORD>(w.size()));
            return w;
        }
#else
        std::string NormalizePosixCompareKey(const fs::path& input)
        {
            std::error_code ec;
            fs::path absP = fs::weakly_canonical(fs::absolute(input, ec), ec);
            if (ec || absP.empty())
                absP = input.lexically_normal();
            std::string s = absP.lexically_normal().string();
            while (s.size() > 1 && s.back() == '/')
                s.pop_back();
            return s;
        }
#endif
    }

    bool PathsReferToSameFile(const std::string& utf8A, const std::string& utf8B)
    {
        if (utf8A.empty() || utf8B.empty())
            return false;

        const fs::path pA = MiniCAD::Utf8ToPath(utf8A);
        const fs::path pB = MiniCAD::Utf8ToPath(utf8B);

        std::error_code ecA, ecB;
        const bool existsA = fs::exists(pA, ecA) && !ecA;
        const bool existsB = fs::exists(pB, ecB) && !ecB;

        if (existsA && existsB)
        {
#ifdef _WIN32
            const auto idA = QueryWinFileIdentity(pA);
            const auto idB = QueryWinFileIdentity(pB);
            if (idA.Valid && idB.Valid)
                return idA == idB;
#endif
            std::error_code eqEc;
            if (fs::equivalent(pA, pB, eqEc) && !eqEc)
                return true;
        }

#ifdef _WIN32
        return NormalizeWinCompareKey(pA) == NormalizeWinCompareKey(pB);
#else
        return NormalizePosixCompareKey(pA) == NormalizePosixCompareKey(pB);
#endif
    }

    bool ValidateIoPathConflicts(const std::vector<PathGuardEntry>& inputs,
                                 const std::vector<PathGuardEntry>& outputs,
                                 std::string* errorCode,
                                 std::string* errorMsg)
    {
        std::vector<PathGuardEntry> activeOutputs;
        activeOutputs.reserve(outputs.size());
        for (const auto& o : outputs)
        {
            if (o.Path.empty())
                continue;
            std::error_code ec;
            const fs::path p = MiniCAD::Utf8ToPath(o.Path);
            if (fs::exists(p, ec) && !ec && fs::is_directory(p, ec) && !ec)
            {
                if (errorCode) *errorCode = "OUTPUT_PATH_CONFLICT";
                if (errorMsg)  *errorMsg  = o.Label + " 指向已存在的目录而非文件：" + o.Path;
                return false;
            }
            activeOutputs.push_back(o);
        }

        for (const auto& in : inputs)
        {
            if (in.Path.empty())
                continue;
            for (const auto& out : activeOutputs)
            {
                if (PathsReferToSameFile(in.Path, out.Path))
                {
                    if (errorCode) *errorCode = "OUTPUT_PATH_CONFLICT";
                    if (errorMsg)
                    {
                        *errorMsg = out.Label + " 与 " + in.Label + " 指向同一文件，已中止以防止覆盖源文件：" + out.Path;
                    }
                    return false;
                }
            }
        }

        for (size_t i = 0; i < activeOutputs.size(); ++i)
        {
            for (size_t j = i + 1; j < activeOutputs.size(); ++j)
            {
                if (PathsReferToSameFile(activeOutputs[i].Path, activeOutputs[j].Path))
                {
                    if (errorCode) *errorCode = "OUTPUT_PATH_CONFLICT";
                    if (errorMsg)
                    {
                        *errorMsg = activeOutputs[i].Label + " 与 " + activeOutputs[j].Label
                                  + " 指向同一输出文件，已中止以防止互相覆盖：" + activeOutputs[i].Path;
                    }
                    return false;
                }
            }
        }

        return true;
    }
}
