#include "CliArgs.h"
#include "CpuRasterizer.h"
#include "DiffEngine.h"
#include "JsonWriter.h"
#include "SqliteExporter.h"
#include "Core/Log.h"
#include "sqlite3.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

namespace
{
    using namespace MiniCAD::SaTool;

    void WriteStdoutLine(const std::string& text)
    {
        if (!text.empty())
            std::fwrite(text.data(), 1, text.size(), stdout);
        if (text.empty() || text.back() != '\n')
            std::fputc('\n', stdout);
        std::fflush(stdout);
    }

    int EmitError(const std::string& code, const std::string& message, int exitCode, bool pretty)
    {
        JsonWriter j(pretty);
        j.BeginObject();
        j.KeyBool("ok", false);
        j.KeyString("error_code", code.empty() ? "RUNTIME_ERROR" : code);
        j.KeyString("error", message.empty() ? "执行失败" : message);
        j.EndObject();
        WriteStdoutLine(j.Take());

        std::fprintf(stderr, "SaDwgTool [%s]: %s\n",
                     code.empty() ? "RUNTIME_ERROR" : code.c_str(),
                     message.c_str());
        std::fflush(stderr);
        return exitCode;
    }

    int RunMainUtf8(const std::vector<std::string>& args)
    {
        // 首行静默常规日志，杜绝任何非 JSON 杂音混入 stdout
        LOG_SET_LEVEL(MiniCAD::LogLevel::Error);

        ParsedCommand cmd;
        std::string errCode, errMsg;
        if (!ParseCliArgs(args, cmd, errCode, errMsg))
            return EmitError(errCode, errMsg, 2, false);

        if (cmd.Verbose)
            LOG_SET_LEVEL(MiniCAD::LogLevel::Info);

        try
        {
            switch (cmd.Kind)
            {
            case CommandKind::Version:
            {
                const std::string line = std::string("SaDwgTool ") + MINICAD_TOOL_VERSION
                                       + " (schema 1.0, sqlite " + sqlite3_libversion() + ")";
                WriteStdoutLine(line);
                return 0;
            }
            case CommandKind::Help:
            {
                WriteStdoutLine(BuildUsageText(cmd.HelpTopic));
                return 0;
            }
            case CommandKind::ExportDb:
            {
                ExportDbResult res;
                std::string code, err;
                bool usageErr = false;
                if (!RunExportDbCommand(cmd.ExportDb, res, &err, &code, &usageErr))
                    return EmitError(code.empty() ? "EXPORT_DB_FAILED" : code, err, usageErr ? 2 : 1, cmd.ExportDb.Pretty);
                WriteStdoutLine(FormatExportDbResultJson(res, cmd.ExportDb.Pretty));
                return 0;
            }
            case CommandKind::Render:
            {
                RenderResult res;
                std::string code, err;
                bool usageErr = false;
                if (!RunRenderCommand(cmd.Render, res, &code, &err, &usageErr))
                    return EmitError(code, err, usageErr ? 2 : 1, cmd.Render.Pretty);
                WriteStdoutLine(FormatRenderResultJson(res, cmd.Render.Pretty));
                return 0;
            }
            case CommandKind::Diff:
            {
                DiffReport report;
                std::string code, err;
                bool usageErr = false;
                if (!RunDiffCommand(cmd.Diff, report, &code, &err, &usageErr))
                    return EmitError(code, err, usageErr ? 2 : 1, cmd.Diff.Pretty);
                const bool compactStdout = !report.JsonPath.empty();
                WriteStdoutLine(FormatDiffReportJson(report, compactStdout, cmd.Diff.Pretty));
                return 0;
            }
            default:
                return EmitError("INVALID_ARGS", "缺少子命令（支持 export-db / render / diff / --version / --help）", 2, false);
            }
        }
        catch (const std::exception& ex)
        {
            return EmitError("UNHANDLED_EXCEPTION", std::string("内部异常：") + ex.what(), 1, cmd.Pretty);
        }
        catch (...)
        {
            return EmitError("UNHANDLED_EXCEPTION", "发生未知内部异常", 1, cmd.Pretty);
        }
    }
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_BINARY);

    std::vector<std::string> utf8Args;
    if (argc > 1)
        utf8Args.reserve(static_cast<size_t>(argc - 1));
    for (int i = 1; i < argc; ++i)
    {
        const wchar_t* w = argv[i];
        if (!w || !*w)
        {
            utf8Args.emplace_back();
            continue;
        }
        std::string s;
        unsigned long winErr = 0;
        if (!WideArgToUtf8(w, s, &winErr))
        {
            return EmitError("INVALID_ARGS",
                             "命令行参数 UTF-8 转码失败（Win32 error " + std::to_string(winErr) + "）",
                             2, false);
        }
        utf8Args.push_back(std::move(s));
    }
    return RunMainUtf8(utf8Args);
}
#else
int main(int argc, char** argv)
{
    std::vector<std::string> utf8Args;
    for (int i = 1; i < argc; ++i)
        utf8Args.emplace_back(argv[i] ? argv[i] : "");
    return RunMainUtf8(utf8Args);
}
#endif
