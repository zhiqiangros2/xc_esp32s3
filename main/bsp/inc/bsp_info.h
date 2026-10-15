#ifndef BOARD_BSP_INFO_H
#define BOARD_BSP_INFO_H

#include <stdint.h>

#define BSP_SOFTWARE_VERSION_SIZE 3U

/**
 * @brief 打印芯片、Flash 和各类内存信息，并执行 1 MiB PSRAM 读写测试。
 *
 * 函数会输出芯片特性、硅版本、Flash 类型和容量、IROM/DROM 程序映射区、
 * IRAM/DRAM 可分配区、PSRAM 容量以及最小剩余堆空间；检测到的 Flash 或
 * PSRAM 容量与 BOX3 预期值不一致时会输出警告。
 */
void bsp_info_print(void);

/**
 * @brief 获取协议使用的系统和 App 软件版本。
 *
 * 每个版本按 patch、minor、major 顺序输出。例如 0.1.2 输出为 02 01 00。
 */
void bsp_info_get_software_versions(
    uint8_t system_version[BSP_SOFTWARE_VERSION_SIZE],
    uint8_t app_version[BSP_SOFTWARE_VERSION_SIZE]);

#endif
