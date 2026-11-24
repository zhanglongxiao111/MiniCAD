// 无界面读图纸文字工具（Node 版 WASM，NODERAWFS 直接读本机文件）。
// 用法：node SaDwgText.js <file.dwg>；TSV 流式写到 stdout（格式见 Document/SaTextExport.h），错误写 stderr、退出码非 0。
#include "Document/Document.h"
#include "Document/SaTextExport.h"
#include <cstdio>
#include <string_view>
using namespace MiniCAD;

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: SaDwgText <file.dwg>\n"); return 2; }
    Document doc;
    std::string err;
    if (!doc.LoadFromFile(argv[1], &err)) { std::fprintf(stderr, "打开图纸失败：%s\n", err.c_str()); return 1; }
    const SaTextExport::Budget budget{};
    const SaTextExport::Result res = SaTextExport::ExportTsvStream(
        doc.GetScene(),
        [](std::string_view chunk) {
            return chunk.empty() || std::fwrite(chunk.data(), 1, chunk.size(), stdout) == chunk.size();
        },
        budget);
    if (!res.Ok())
    {
        switch (res.Code)
        {
        case SaTextExport::Status::ArrayCellBudgetExceeded:
            std::fprintf(stderr, "导出文字超出阵列单元展开预算（上限 %zu 单元）\n", budget.MaxArrayCells);
            return 3;
        case SaTextExport::Status::RowBudgetExceeded:
            std::fprintf(stderr, "导出文字超出最大文字行数预算（上限 %zu 行）\n", budget.MaxRows);
            return 3;
        case SaTextExport::Status::ByteBudgetExceeded:
            std::fprintf(stderr, "导出文字超出最大输出字节预算（上限 %zu 字节）\n", budget.MaxBytes);
            return 3;
        case SaTextExport::Status::SinkError:
            std::fprintf(stderr, "写入标准输出失败\n");
            return 1;
        case SaTextExport::Status::Ok:
            break;
        }
    }
    if (std::fflush(stdout) != 0)
    {
        std::fprintf(stderr, "刷新标准输出失败\n");
        return 1;
    }
    return 0;
}
