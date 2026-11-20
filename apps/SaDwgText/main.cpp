// 无界面读图纸文字工具（Node 版 WASM，NODERAWFS 直接读本机文件）。
// 用法：node SaDwgText.js <file.dwg>；TSV 写到 stdout（格式见 Document/SaTextExport.h），错误写 stderr、退出码非 0。
#include "Document/Document.h"
#include "Document/SaTextExport.h"
#include <cstdio>
using namespace MiniCAD;

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: SaDwgText <file.dwg>\n"); return 2; }
    Document doc;
    std::string err;
    if (!doc.LoadFromFile(argv[1], &err)) { std::fprintf(stderr, "打开图纸失败：%s\n", err.c_str()); return 1; }
    const std::string tsv = SaTextExport::ExportTsv(doc.GetScene());
    std::fwrite(tsv.data(), 1, tsv.size(), stdout);
    return 0;
}
