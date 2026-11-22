# SQLite 3 Amalgamation

- 来源：https://www.sqlite.org/download.html（`sqlite-amalgamation-3540100.zip`）
- 版本：3.54.1（2026-10-16，`SQLITE_VERSION_NUMBER = 3054001`）
- 许可证：Public Domain（公共领域），见 `sqlite3.h` 文件头声明
- 用途：供无头命令行工具 `SaDwgTool` 导出带 `R-tree` 空间索引（`SQLITE_ENABLE_RTREE`）的标准 SQLite 数据库，零外部动态库依赖
- 本地修改：无（原样保留官方单文件合并源码 `sqlite3.c` 与 `sqlite3.h`）
