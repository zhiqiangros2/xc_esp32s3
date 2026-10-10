# 中文字体说明

## 文件

- `font.ttf`：Noto Sans Simplified Chinese 可变字体，用于 FreeType 显示中文。
- `font-OFL.txt`：字体对应的 SIL Open Font License 1.1 许可证。

## 下载来源

字体来自 Google Fonts 官方仓库：

- 字体页面：<https://github.com/google/fonts/blob/main/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf>
- 字体直接下载地址：<https://raw.githubusercontent.com/google/fonts/main/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf>
- 许可证页面：<https://github.com/google/fonts/blob/main/ofl/notosanssc/OFL.txt>
- 许可证直接下载地址：<https://raw.githubusercontent.com/google/fonts/main/ofl/notosanssc/OFL.txt>

当前字体文件信息：

- 文件大小：17,772,300 字节
- SHA-256：`A3041811A78C361B1DE50F953C805E0244951C21C5BD412F7232EF0D899AF0DA`

## SD 卡路径

在 SD 卡根目录创建 `font` 文件夹，将 `font.ttf` 复制进去：

```text
SD卡:\font\font.ttf
```

SD 卡挂载到 ESP-IDF VFS 后，程序通过以下路径加载字体：

```text
/sdcard/font/font.ttf
```

字体由 `main/app/src/lvgl_font.c` 中的 FreeType 加载代码读取，不会从
`components/font/font.ttf` 自动复制到 SD 卡，也不会被编译进应用固件。

## LVGL 9.6 配置

本项目使用的 LVGL 9.6 不支持按字节配置的 `LV_FREETYPE_CACHE_SIZE`。对应的
有效配置是按字形数量限制缓存：

```text
CONFIG_LV_TXT_ENC_UTF8=y
CONFIG_LV_USE_FREETYPE=y
CONFIG_LV_FREETYPE_CACHE_FT_GLYPH_CNT=256
CONFIG_LV_USE_CLIB_MALLOC=y
```

当前字号为 18 px，256 个缓存字形的总体规模约为 128 KB。实际占用会随字符
尺寸变化，不是固定的 128 KB。LVGL 使用 ESP-IDF 标准堆，不能使用默认的
64 KiB 固定池，否则反复创建中文页面后会耗尽字体缓存节点空间。根据工程的
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` 配置，16 KiB 及以上的 LVGL 分配
会优先进入 PSRAM。FreeType 使用官方 `ftsystem.c` 和 ESP-IDF 默认
`malloc/realloc/free`：小于等于 16 KiB 的分配优先使用内部 RAM，更大的分配
优先使用 PSRAM；首选内存不足时由 ESP-IDF 堆分配器尝试另一类内存。
