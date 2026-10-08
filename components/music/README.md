# 音乐文件

本目录用于暂存准备复制到 SD 卡的音频文件。这些文件不会编译进固件。

## 文件信息

| 原文件名 | 格式 | 时长 | 大小 | SHA-256 |
| --- | --- | ---: | ---: | --- |
| `周笔畅-最美的期待.WAV` | PCM、双声道、48 kHz、16 bit | 03:30.867 | 40,486,446 字节 | `7A85A5C56765CD365135A8EC9DE7AE73700F74BA692F73E0FC17CA362C9FE503` |
| `风起天阑.WAV` | PCM、双声道、48 kHz、16 bit | 05:28.067 | 62,988,846 字节 | `6848708A4C86E413E6744CF217B122DB77C26C83EBFA0C59852F859947FCA5FC` |

两个文件均为未压缩 WAV，数据速率为 192,000 字节/秒，文件长度及音频帧对齐检查正常。

## 复制到 SD 卡

建议在 SD 卡根目录创建 `music` 文件夹，并使用 ASCII 短文件名：

```text
/sdcard/music/MUSIC01.WAV
/sdcard/music/MUSIC02.WAV
```

建议的文件名对应关系：

```text
周笔畅-最美的期待.WAV -> MUSIC01.WAV
风起天阑.WAV          -> MUSIC02.WAV
```

当前工程配置为 `CONFIG_FATFS_LFN_NONE=y`，不支持按中文长文件名访问文件。若需要保留中文文件名，应改为启用：

```ini
CONFIG_FATFS_LFN_HEAP=y
CONFIG_FATFS_API_ENCODING_UTF_8=y
```

## 播放代码注意事项

这两个 WAV 的 `fmt ` 数据块长度为 18 字节，音频数据从文件偏移 46 字节开始。播放器应按 RIFF 数据块结构查找 `data`，不要固定跳过 44 字节。

当前工程尚未实现 WAV 播放、I2S 输出和音频芯片驱动。将文件复制到 SD 卡后，还需要播放器代码才能输出声音。
