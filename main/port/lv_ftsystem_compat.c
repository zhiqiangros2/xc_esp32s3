#include <stddef.h>
#include <stdlib.h>

#include "esp_heap_caps.h"

static void *freetype_psram_malloc(size_t size)
{
    void *block = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (block == NULL) {
        block = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return block;
}

static void *freetype_psram_realloc(void *block, size_t size)
{
    void *resized = heap_caps_realloc(block,
                                      size,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (resized == NULL) {
        resized = heap_caps_realloc(block, size, MALLOC_CAP_8BIT);
    }
    return resized;
}

static void freetype_psram_free(void *block)
{
    heap_caps_free(block);
}

/*
 * 只替换 FreeType 系统层使用的内存函数，文件访问仍使用 FreeType 官方的
 * ANSI FILE 流。这样不会经过 LVGL 的文件缓存，字体数据仍由 SD 卡按需读取。
 * stdlib.h 已在上面包含，以下宏不会改写标准库函数声明。
 */
#define malloc(size) freetype_psram_malloc(size)
#define realloc(block, size) freetype_psram_realloc((block), (size))
#define free(block) freetype_psram_free(block)

#include "../../components/espressif__freetype/freetype/src/base/ftsystem.c"

#undef malloc
#undef realloc
#undef free
