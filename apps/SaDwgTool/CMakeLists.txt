# ── SaDwgTool：原生 64 位无头 DWG/DXF 解析、SQLite 导出、CPU 渲染与三轨差分引擎 ──
# 零 MiniGUI / 零 GPU / 零窗口依赖，静态链接 CRT（/MT）。

add_library(SaDwgToolCore STATIC
    "CliArgs.cpp"
    "PngWriter.cpp"
    "TchSemanticExport.cpp"
    "SqliteExporter.cpp"
    "SceneTessellator.cpp"
    "CpuRasterizer.cpp"
    "DiffEngine.cpp"
)
target_include_directories(SaDwgToolCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(SaDwgToolCore PUBLIC MiniCAD sqlite3 PRIVATE MiniDWG)
target_compile_definitions(SaDwgToolCore PUBLIC
    MINICAD_TOOL_VERSION="${MINICAD_VERSION}"
)
if(MSVC)
    target_compile_options(SaDwgToolCore PRIVATE /utf-8 /W4 /bigobj)
    target_compile_definitions(SaDwgToolCore PRIVATE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

add_executable(SaDwgTool "main.cpp")
target_link_libraries(SaDwgTool PRIVATE SaDwgToolCore)
if(MSVC)
    target_compile_options(SaDwgTool PRIVATE /utf-8 /W4)
    target_compile_definitions(SaDwgTool PRIVATE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
set_target_properties(SaDwgTool PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
)
