/**
 * @file lvgl_memory.c
 * @brief 将 LVGL 自身的动态对象明确分配到 PSRAM。
 *
 * LVGL 9 的对象、样式、定时器、事件描述符和文本副本通常都是几百字节的小块。
 * 如果直接使用标准 malloc，这些分配仍可能持续消耗片内堆，最终无法再创建
 * 任务控制块或驱动对象，因此 LVGL 核心分配全部固定到 PSRAM。FreeType 当前
 * 使用标准 malloc，其中大于项目阈值的解析和缓存分配同样优先进入 PSRAM。
 *
 * 这里不提供内部 RAM 回退。PSRAM 不足时让 LVGL 收到 NULL 并按原有错误路径
 * 处理，不能为了创建界面而挤占 I2S DMA、I2C、SPI 和 FreeRTOS 所需内部内存。
 */
#include "lvgl.h"

#include "esp_heap_caps.h"

static const uint32_t LVGL_MEMORY_CAPS =
    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

void lv_mem_init(void)
{
    /* ESP-IDF 在 app_main() 前已经初始化 PSRAM 堆，无需建立额外内存池。 */
}

void lv_mem_deinit(void)
{
    /* 每个 LVGL 对象仍由 lv_free() 按生命周期释放，没有整体内存池可销毁。 */
}

lv_mem_pool_t lv_mem_add_pool(void *memory, size_t size)
{
    /* heap_caps 分配器不支持把调用者提供的连续区域追加为独立 LVGL 内存池。 */
    (void)memory;
    (void)size;
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    (void)pool;
}

void *lv_malloc_core(size_t size)
{
    return heap_caps_malloc(size, LVGL_MEMORY_CAPS);
}

void *lv_realloc_core(void *memory, size_t new_size)
{
    return heap_caps_realloc(memory, new_size, LVGL_MEMORY_CAPS);
}

void lv_free_core(void *memory)
{
    heap_caps_free(memory);
}

void lv_mem_monitor_core(lv_mem_monitor_t *monitor)
{
    const size_t total_size = heap_caps_get_total_size(LVGL_MEMORY_CAPS);
    const size_t free_size = heap_caps_get_free_size(LVGL_MEMORY_CAPS);
    const size_t minimum_free =
        heap_caps_get_minimum_free_size(LVGL_MEMORY_CAPS);
    const size_t largest_free =
        heap_caps_get_largest_free_block(LVGL_MEMORY_CAPS);

    monitor->total_size = total_size;
    monitor->free_size = free_size;
    monitor->free_biggest_size = largest_free;
    monitor->max_used = total_size - minimum_free;
    monitor->used_pct = total_size == 0U
                            ? 0U
                            : (uint8_t)(((total_size - free_size) * 100U) /
                                        total_size);
    monitor->frag_pct = free_size == 0U
                            ? 0U
                            : (uint8_t)(100U -
                                        ((largest_free * 100U) / free_size));
}

lv_result_t lv_mem_test_core(void)
{
    return heap_caps_get_free_size(LVGL_MEMORY_CAPS) > 0U
               ? LV_RESULT_OK
               : LV_RESULT_INVALID;
}
