# ── SQLite3（官方单文件合并源码，供无头工具 SaDwgTool 与测试使用）──────────
set(MINICAD_SQLITE3_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rd/sqlite3")

add_library(sqlite3 STATIC
    "${MINICAD_SQLITE3_DIR}/sqlite3.c"
)
target_include_directories(sqlite3 PUBLIC "${MINICAD_SQLITE3_DIR}")
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_ENABLE_RTREE=1
    SQLITE_ENABLE_FTS5=1
    SQLITE_THREADSAFE=1
    SQLITE_DQS=0
    SQLITE_OMIT_LOAD_EXTENSION=1
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_MAX_EXPR_DEPTH=1000
)
if(MSVC)
    target_compile_options(sqlite3 PRIVATE /utf-8 /W0)
else()
    target_compile_options(sqlite3 PRIVATE -w)
endif()
