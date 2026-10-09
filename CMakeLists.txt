cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(xc_esp32s3_tool)

# FreeType 保留官方文件流，只把系统层内存分配定向到 PSRAM。
if(TARGET freetype)
    get_target_property(FREETYPE_SOURCES freetype SOURCES)
    list(FILTER FREETYPE_SOURCES EXCLUDE REGEX "src/base/ftsystem\\.c$")
    set_property(TARGET freetype PROPERTY SOURCES "${FREETYPE_SOURCES}")

    target_sources(freetype PRIVATE
        "${CMAKE_SOURCE_DIR}/main/port/lv_ftsystem_compat.c"
    )
endif()
