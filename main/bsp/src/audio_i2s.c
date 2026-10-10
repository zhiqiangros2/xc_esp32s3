/**
 * @file audio_i2s.c
 * @brief ESP32-S3、ES8311 和 ES7210 共用的 I2S0 全双工通道封装。
 *
 * ESP32-S3 是总线主机，产生共用的 MCLK、BCLK、WS/LRCK。ES8311 与 ES7210
 * 共用这三根时钟线，但数据线相互独立：GPIO40 是播放 TX（ESP32 -> ES8311），
 * GPIO41 是录音 RX（ES7210 -> ESP32），所以两个方向可以同时传输。
 *
 * I2S0 硬件统一配置成 16 bit、四槽 Philips TDM，主要目的是让真正的四通道
 * TDM 发送端 ES7210 能在一根 SDOUT 上返回四路 ADC。一个 LRCK 周期的物理
 * 布局如下，每槽 16 bit，整帧 64 BCLK：
 *
 *   WS/LRCK： 低电平（32 BCLK）       高电平（32 BCLK）
 *   槽号：     slot0       slot1       slot2       slot3
 *   播放 TX：  L           0           R           0
 *   录音 RX：  MIC1        MIC3        MIC2        MIC4
 *
 * ES8311 本身不是四通道 TDM Codec，而是单 DAC Codec。它仍按普通 16 bit
 * Philips I2S 解码：低、高半周期各形成一个 32 BCLK 的左、右声道窗口，只
 * 读取每个窗口开头的 16 bit，后 16 bit 是填充。因此播放器必须把 [L,R]
 * 展开为 [L,0,R,0]；写成 [L,R,0,0] 会把 R 放进左窗口的填充位置，并使右
 * 窗口读到 0。当前 ES8311 寄存器选择左声道送入单 DAC，所以实际模拟输出
 * 使用 slot0 的 L；slot2 的 R 只是保持串行总线左右窗口位置正确，并不会
 * 形成第二路模拟输出。需要左右声道都参与单声道播放时，应先在上层下混为
 * M=(L+R)/2，再发送 [M,M]，本驱动不隐式改变音频内容。
 *
 * 48 kHz 时：LRCK=48 kHz，BCLK=48000 x 4 x 16=3.072 MHz（64fs），
 * MCLK=48000 x 256=12.288 MHz。
 *
 * 两个方向共用时钟，但 DMA TX 和 DMA RX 可以同时运行：控制操作由控制锁
 * 保护，阻塞的发送和接收分别使用独立锁，避免一次 RX 等待阻塞播放。
 *
 * 调用顺序：
 * 1. audio_i2s_init() 创建 TX/RX，只启用 TX 以持续输出 Codec 时钟；
 * 2. 初始化 ES8311 和 ES7210；
 * 3. 播放使用 playback_start/write/playback_stop，录音使用
 *    record_start/read/record_stop。
 */
#include "audio_i2s.h"

#include <inttypes.h>

#include "driver/gpio.h"
#include "driver/i2s_tdm.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* BOX3 原理图固定使用 I2S0，同时连接板载 ES8311 与 ES7210。 */
#define AUDIO_I2S_PORT I2S_NUM_0
/*
 * Codec 初始化前用于产生稳定 MCLK/BCLK/WS 的启动采样率。实际播放时，
 * wav_player 会根据文件头调用 audio_i2s_playback_start() 切换到文件采样率。
 */
#define AUDIO_I2S_STARTUP_SAMPLE_RATE_HZ 48000U
/* 公共主时钟 MCLK：同时送往 ES8311 与 ES7210 的内部时钟电路。 */
#define AUDIO_I2S_MCLK_GPIO GPIO_NUM_21
/* 位时钟 BCLK：每传输一个 PCM 数据位跳变一次。 */
#define AUDIO_I2S_BCLK_GPIO GPIO_NUM_38
/* 左右声道选择时钟 WS/LRCK：频率等于音频采样率。 */
#define AUDIO_I2S_WS_GPIO GPIO_NUM_39
/* ESP32-S3 播放数据输出，连接 ES8311 的串行音频输入 SDIN。 */
#define AUDIO_I2S_DOUT_GPIO GPIO_NUM_40
/* ES7210 的串行录音数据输出连接到 ESP32-S3 的 I2S DIN。 */
#define AUDIO_I2S_DIN_GPIO GPIO_NUM_41
/*
 * dma_frame_num 的“一帧”包含四个 16 bit TDM 槽：
 *   单声道采样大小 = 16 bit / 8 = 2 字节；
 *   一帧大小       = 2 字节 x 4 槽 = 8 字节；
 *   单个 DMA 数据缓冲区 = 511 帧 x 8 字节 = 4088 字节；
 *   单方向 12 个缓冲区  = 4088 字节 x 12 = 49056 字节；
 *   TX + RX 数据区合计  = 49056 字节 x 2 = 98112 字节。
 *
 * 这里是“每个 DMA 描述符关联一个 4088 字节的数据缓冲区”，并不是描述符
 * 结构本身占用 4088 字节。48 kHz 四槽数据每秒为
 * 48000 帧 x 8 字节 = 384000 字节，所以每个方向的 DMA 缓冲可维持：
 *   49056 / 384000 = 0.12775 秒，约 128 ms。
 *
 * 这段时间用于吸收 SD 卡读取、SPI 总线占用和界面刷新造成的短时调度抖动。
 * 511 帧还可确保单个 DMA 数据块不超过 ESP-IDF 要求的 4092 字节上限。
 */
#define AUDIO_I2S_DMA_DESCRIPTOR_COUNT 12
#define AUDIO_I2S_DMA_FRAME_COUNT 511
/* audio_i2s_write() 每轮最多转换 4096 字节普通双声道 PCM。 */
#define AUDIO_I2S_STEREO_WRITE_CHUNK_SIZE 4096U
#define AUDIO_I2S_STEREO_FRAME_SIZE 4U
#define AUDIO_I2S_TDM_FRAME_SIZE 8U

/* 非 NULL 表示 I2S 通道已经创建；句柄由 ESP-IDF I2S 驱动拥有。 */
static i2s_chan_handle_t s_tx_channel;
/* ES7210 录音接收通道，与 TX 共用 MCLK/BCLK/WS。 */
static i2s_chan_handle_t s_rx_channel;
/* 区分“通道已创建”和“通道正在运行”，用于失败后的再次启用。 */
static bool s_tx_channel_enabled;
static bool s_rx_channel_enabled;
/* 记录当前硬件采样率，避免重复停止并重配时钟。 */
static uint32_t s_sample_rate_hz;
/* true 表示播放器已开始使用公共时钟；TX 硬件在空闲时仍保持开启并输出静音。 */
static bool s_playback_active;
/* 只保护通道状态、播放/录音状态和采样率等短控制操作。 */
static SemaphoreHandle_t s_control_mutex;
/* 分别串行化 TX 写和 RX 读；两个锁互不影响，所以播放与录音可同时阻塞。 */
static SemaphoreHandle_t s_tx_mutex;
static SemaphoreHandle_t s_rx_mutex;
/* 4096 字节 [L,R] 展开后是 8192 字节 [L,0,R,0]，仅由 TX 锁内使用。 */
static int16_t s_tx_tdm_buffer[AUDIO_I2S_STEREO_WRITE_CHUNK_SIZE];
static const char *TAG = "AUDIO_I2S";

/** 删除初始化阶段已经创建的互斥锁，并把句柄恢复为空。 */
static void delete_mutexes(void)
{
    if (s_rx_mutex != NULL) {
        vSemaphoreDelete(s_rx_mutex);
        s_rx_mutex = NULL;
    }
    if (s_tx_mutex != NULL) {
        vSemaphoreDelete(s_tx_mutex);
        s_tx_mutex = NULL;
    }
    if (s_control_mutex != NULL) {
        vSemaphoreDelete(s_control_mutex);
        s_control_mutex = NULL;
    }
}

/**
 * @brief 创建并启动 I2S0 四槽 TDM 全双工通道。
 *
 * 配置固定为 Philips I2S、16 bit、四槽、ESP32-S3 主机模式。本函数只在
 * main.c 的板级启动流程中调用一次；播放不同采样率文件时使用
 * audio_i2s_playback_start()，不重复创建硬件通道。
 *
 * @return ESP_OK 成功；其他值来自 I2S 驱动的通道创建、配置或启动阶段。
 */
esp_err_t audio_i2s_init(void)
{
    if (s_tx_channel != NULL || s_rx_channel != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_control_mutex = xSemaphoreCreateMutex();
    s_tx_mutex = xSemaphoreCreateMutex();
    s_rx_mutex = xSemaphoreCreateMutex();
    if (s_control_mutex == NULL || s_tx_mutex == NULL ||
        s_rx_mutex == NULL) {
        delete_mutexes();
        return ESP_ERR_NO_MEM;
    }

    /*
     * 同时创建 TX/RX 全双工通道。auto_clear 会在发送下溢时自动输出 0，
     * 避免重复播放 DMA 中的旧采样。TX 始终开启并负责提供公共 Codec 时钟，
     * RX 只在实际录音期间启用。
     */
    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = AUDIO_I2S_DMA_DESCRIPTOR_COUNT;
    channel_config.dma_frame_num = AUDIO_I2S_DMA_FRAME_COUNT;
    channel_config.auto_clear = true;

    esp_err_t result = i2s_new_channel(&channel_config,
                                       &s_tx_channel,
                                       &s_rx_channel);
    if (result != ESP_OK) {
        ESP_LOGE(TAG,
                 "i2s_new_channel(I2S0 TX/RX) failed: %s",
                 esp_err_to_name(result));
        s_tx_channel = NULL;
        s_rx_channel = NULL;
        delete_mutexes();
        return result;
    }
    s_tx_channel_enabled = false;
    s_rx_channel_enabled = false;

    /*
     * 四个槽全部启用且不跳槽，DMA 中每帧始终是四个连续 int16_t。自动 WS
     * 宽度为半帧，即低 32 BCLK、高 32 BCLK；Philips 模式仍保留 1 BCLK
     * 的 MSB 延迟。
     *
     * I2S_SLOT_MODE_STEREO 在这里定义的是 WS 具有低/高两个相位，并不表示
     * DMA 只有两个槽；slot_mask 明确启用了 slot0～slot3，因此 total_slot
     * 是 4。TX/RX 属于同一个 I2S0 全双工控制器，必须使用完全相同的时钟、
     * WS 和槽配置：TX 用 [L,0,R,0]，RX 接收 [MIC1,MIC3,MIC2,MIC4]。
     */
    const i2s_tdm_config_t tdm_config = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(
            AUDIO_I2S_STARTUP_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_STEREO,
            I2S_TDM_SLOT0 | I2S_TDM_SLOT1 |
                I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
        .gpio_cfg = {
            .mclk = AUDIO_I2S_MCLK_GPIO,
            .bclk = AUDIO_I2S_BCLK_GPIO,
            .ws = AUDIO_I2S_WS_GPIO,
            .dout = AUDIO_I2S_DOUT_GPIO,
            .din = AUDIO_I2S_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    /* 先把两个方向配置成同一 TDM 模式，再启用 TX 输出公共 Codec 时钟。 */
    result = i2s_channel_init_tdm_mode(s_tx_channel, &tdm_config);
    if (result != ESP_OK) {
        ESP_LOGE(TAG,
                 "i2s_channel_init_tdm_mode(TX) failed: %s",
                 esp_err_to_name(result));
    }
    if (result == ESP_OK) {
        result = i2s_channel_init_tdm_mode(s_rx_channel, &tdm_config);
        if (result != ESP_OK) {
            ESP_LOGE(TAG,
                     "i2s_channel_init_tdm_mode(RX) failed: %s",
                     esp_err_to_name(result));
        }
    }
    if (result == ESP_OK) {
        result = i2s_channel_enable(s_tx_channel);
        if (result != ESP_OK) {
            ESP_LOGE(TAG,
                     "i2s_channel_enable failed: %s",
                     esp_err_to_name(result));
        }
    }
    /* 任一阶段失败都删除已创建通道，下一次调用可以从干净状态重试。 */
    if (result != ESP_OK) {
        i2s_del_channel(s_rx_channel);
        i2s_del_channel(s_tx_channel);
        s_rx_channel = NULL;
        s_tx_channel = NULL;
        s_tx_channel_enabled = false;
        s_rx_channel_enabled = false;
        delete_mutexes();
        return result;
    }

    s_sample_rate_hz = AUDIO_I2S_STARTUP_SAMPLE_RATE_HZ;
    s_tx_channel_enabled = true;
    s_playback_active = false;
    ESP_LOGI(TAG,
             "I2S0 TX/RX ready: %" PRIu32 " Hz, 16-bit, 4-slot TDM",
             AUDIO_I2S_STARTUP_SAMPLE_RATE_HZ);
    return ESP_OK;
}

/**
 * @brief 在保留 DMA 通道的情况下修改 I2S 采样率。
 *
 * ESP-IDF 要求 TDM 模式时钟只能在通道停止时重配。采样率未变化且通道正在
 * 运行时直接返回；通道曾启动失败时只重试启用；采样率变化时执行“停用、
 * 重配、重新启用”。重配失败仍尝试恢复旧配置对应的通道运行状态。
 *
 * @param sample_rate_hz 新采样率，必须大于 0。
 * @return ESP_OK 成功；否则返回参数检查或 I2S 驱动错误。
 */
static esp_err_t set_sample_rate_locked(uint32_t sample_rate_hz)
{
    const bool sample_rate_changed = sample_rate_hz != s_sample_rate_hz;
    /* RX 工作时修改公共时钟会破坏 WAV 采样率，只允许保持当前采样率。 */
    if (sample_rate_changed && s_rx_channel_enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!sample_rate_changed && s_tx_channel_enabled) {
        return ESP_OK;
    }

    /* 只有正在运行且确实需要重配时钟时才停用通道。 */
    if (sample_rate_changed && s_tx_channel_enabled) {
        const esp_err_t disable_result = i2s_channel_disable(s_tx_channel);
        if (disable_result != ESP_OK) {
            ESP_LOGE(TAG,
                     "Cannot stop I2S before changing sample rate: %s",
                     esp_err_to_name(disable_result));
            return disable_result;
        }
        s_tx_channel_enabled = false;
    }

    esp_err_t reconfigure_result = ESP_OK;
    if (sample_rate_changed) {
        const i2s_tdm_clk_config_t clock_config =
            I2S_TDM_CLK_DEFAULT_CONFIG(sample_rate_hz);
        reconfigure_result =
            i2s_channel_reconfig_tdm_clock(s_tx_channel, &clock_config);
        if (reconfigure_result == ESP_OK) {
            reconfigure_result =
                i2s_channel_reconfig_tdm_clock(s_rx_channel, &clock_config);
        }
        if (reconfigure_result == ESP_OK) {
            s_sample_rate_hz = sample_rate_hz;
        } else {
            ESP_LOGE(TAG,
                     "Cannot set I2S TX/RX sample rate to %" PRIu32 " Hz: %s",
                     sample_rate_hz,
                     esp_err_to_name(reconfigure_result));

            /* RX 重配失败时把两个通道都尽量恢复到旧时钟，避免 TX/RX 配置不一致。 */
            const i2s_tdm_clk_config_t old_clock_config =
                I2S_TDM_CLK_DEFAULT_CONFIG(s_sample_rate_hz);
            i2s_channel_reconfig_tdm_clock(s_tx_channel, &old_clock_config);
            i2s_channel_reconfig_tdm_clock(s_rx_channel, &old_clock_config);
        }
    }

    /* 无论重配是否成功都尝试启动通道；失败时下次调用会再次执行 enable。 */
    const esp_err_t enable_result = i2s_channel_enable(s_tx_channel);
    if (enable_result == ESP_OK) {
        s_tx_channel_enabled = true;
    } else {
        s_tx_channel_enabled = false;
        ESP_LOGE(TAG,
                 "Cannot enable I2S TX channel: %s",
                 esp_err_to_name(enable_result));
    }
    return reconfigure_result != ESP_OK ? reconfigure_result : enable_result;
}

esp_err_t audio_i2s_playback_start(uint32_t sample_rate_hz)
{
    if (sample_rate_hz == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_tx_channel == NULL || s_rx_channel == NULL ||
        s_control_mutex == NULL || s_tx_mutex == NULL ||
        s_rx_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * 固定取锁顺序为 TX -> RX -> control。重配期间两个方向都不会访问 DMA；
     * 普通播放写和录音读只拿各自方向的锁，因此运行期仍可真正并行。
     */
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    xSemaphoreTake(s_rx_mutex, portMAX_DELAY);
    xSemaphoreTake(s_control_mutex, portMAX_DELAY);
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (!s_playback_active) {
        result = set_sample_rate_locked(sample_rate_hz);
        if (result == ESP_OK) {
            s_playback_active = true;
        }
    }
    xSemaphoreGive(s_control_mutex);
    xSemaphoreGive(s_rx_mutex);
    xSemaphoreGive(s_tx_mutex);
    return result;
}

esp_err_t audio_i2s_playback_stop(void)
{
    if (s_control_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_control_mutex, portMAX_DELAY);
    s_playback_active = false;
    xSemaphoreGive(s_control_mutex);
    return ESP_OK;
}

esp_err_t audio_i2s_record_start(uint32_t sample_rate_hz)
{
    if (sample_rate_hz == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_tx_channel == NULL || s_rx_channel == NULL ||
        s_control_mutex == NULL || s_tx_mutex == NULL ||
        s_rx_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 可能需要改公共时钟，因此与播放启动使用相同的完整取锁顺序。 */
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    xSemaphoreTake(s_rx_mutex, portMAX_DELAY);
    xSemaphoreTake(s_control_mutex, portMAX_DELAY);
    if (s_rx_channel_enabled) {
        const esp_err_t result = sample_rate_hz == s_sample_rate_hz
                                     ? ESP_OK
                                     : ESP_ERR_INVALID_STATE;
        xSemaphoreGive(s_control_mutex);
        xSemaphoreGive(s_rx_mutex);
        xSemaphoreGive(s_tx_mutex);
        return result;
    }

    /* 已播放时不能偷偷改变公共时钟，否则歌曲会变速且录音头部采样率错误。 */
    esp_err_t result = ESP_OK;
    if (s_playback_active && sample_rate_hz != s_sample_rate_hz) {
        ESP_LOGE(TAG,
                 "Cannot start %" PRIu32
                 " Hz recording while %" PRIu32 " Hz playback is active",
                 sample_rate_hz,
                 s_sample_rate_hz);
        result = ESP_ERR_INVALID_STATE;
    } else {
        result = set_sample_rate_locked(sample_rate_hz);
    }
    if (result == ESP_OK) {
        result = i2s_channel_enable(s_rx_channel);
        if (result == ESP_OK) {
            s_rx_channel_enabled = true;
        } else {
            ESP_LOGE(TAG,
                     "Cannot enable I2S RX channel: %s",
                     esp_err_to_name(result));
        }
    }
    xSemaphoreGive(s_control_mutex);
    xSemaphoreGive(s_rx_mutex);
    xSemaphoreGive(s_tx_mutex);
    return result;
}

esp_err_t audio_i2s_record_stop(void)
{
    if (s_rx_channel == NULL || s_control_mutex == NULL ||
        s_rx_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_rx_mutex, portMAX_DELAY);
    xSemaphoreTake(s_control_mutex, portMAX_DELAY);
    esp_err_t result = ESP_OK;
    if (s_rx_channel_enabled) {
        result = i2s_channel_disable(s_rx_channel);
        if (result == ESP_OK) {
            s_rx_channel_enabled = false;
        } else {
            ESP_LOGE(TAG,
                     "Cannot disable I2S RX channel: %s",
                     esp_err_to_name(result));
        }
    }
    xSemaphoreGive(s_control_mutex);
    xSemaphoreGive(s_rx_mutex);
    return result;
}

/**
 * @brief 把普通双声道 PCM 展开为四槽 TDM 后复制到 I2S DMA 队列。
 *
 * 输入每帧是 [L,R] 两个 int16_t。ES8311 仍以 LRCK 的低/高半周期识别
 * 普通 I2S 左右声道窗口，因此转换为 [L,0,R,0]：槽 0/2 分别位于两个
 * 半周期开头，槽 1/3 是 ES8311 不使用的填充时间并固定写 0。
 *
 * ES8311 只有一个 DAC，OUTP/OUTN 是同一单声道的差分正负端，不是 L/R
 * 两路输出。当前寄存器选择左窗口，因此实际播放 slot0；这里仍保留 R 到
 * slot2，以保证线上的普通 I2S 左右位置正确。本函数不执行立体声到单声道
 * 下混。bytes_written 始终按“输入双声道 PCM 字节数”返回，不包含插入的
 * 两个填充槽。
 *
 * @param data 16 bit、双声道交错 PCM 首地址。
 * @param data_size 输入字节数，必须大于 0 且是 4 字节帧的整数倍。
 * @param bytes_written 返回已经完整转换并提交的输入 PCM 字节数。
 * @param timeout_ms 最长等待时间，单位毫秒。
 * @return ESP_OK 全部写入；ESP_ERR_TIMEOUT 超时；或参数、状态错误。
 */
esp_err_t audio_i2s_write(const void *data,
                          size_t data_size,
                          size_t *bytes_written,
                          uint32_t timeout_ms)
{
    if (data == NULL || data_size == 0 || bytes_written == NULL ||
        data_size % AUDIO_I2S_STEREO_FRAME_SIZE != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_control_mutex == NULL || s_tx_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * i2s_channel_write() 的参数单位已经是毫秒，驱动内部会自行调用
     * pdMS_TO_TICKS()。这里必须直接传 timeout_ms；若提前转换一次，在
     * CONFIG_FREERTOS_HZ=100 时，100 ms 会被错误缩短成 10 ms。
     */
    *bytes_written = 0;
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    xSemaphoreTake(s_control_mutex, portMAX_DELAY);
    esp_err_t result = ESP_ERR_INVALID_STATE;
    const bool can_write = s_tx_channel != NULL &&
                           s_tx_channel_enabled &&
                           s_playback_active;
    xSemaphoreGive(s_control_mutex);
    if (can_write) {
        const uint8_t *source = data;
        result = ESP_OK;

        while (*bytes_written < data_size) {
            size_t source_size = data_size - *bytes_written;
            if (source_size > AUDIO_I2S_STEREO_WRITE_CHUNK_SIZE) {
                source_size = AUDIO_I2S_STEREO_WRITE_CHUNK_SIZE;
            }

            /*
             * 一个输入帧 4 字节、一个物理 TDM 帧 8 字节：
             *   [L,R] -> [slot0=L, slot1=0, slot2=R, slot3=0]
             * ES8311 在每个 LRCK 半周期只取第一个 16 bit；slot1/3 是时钟
             * 对齐所需的填充槽，固定清零可避免总线上残留无意义的旧数据。
             */
            const size_t frame_count =
                source_size / AUDIO_I2S_STEREO_FRAME_SIZE;
            const int16_t *stereo =
                (const int16_t *)(source + *bytes_written);
            for (size_t frame = 0; frame < frame_count; ++frame) {
                s_tx_tdm_buffer[frame * 4U] = stereo[frame * 2U];
                s_tx_tdm_buffer[frame * 4U + 1U] = 0;
                s_tx_tdm_buffer[frame * 4U + 2U] =
                    stereo[frame * 2U + 1U];
                s_tx_tdm_buffer[frame * 4U + 3U] = 0;
            }

            const size_t tdm_size = frame_count * AUDIO_I2S_TDM_FRAME_SIZE;
            size_t tdm_offset = 0;
            while (tdm_offset < tdm_size) {
                size_t tdm_bytes_written = 0;
                result = i2s_channel_write(
                    s_tx_channel,
                    (const uint8_t *)s_tx_tdm_buffer + tdm_offset,
                    tdm_size - tdm_offset,
                    &tdm_bytes_written,
                    timeout_ms);
                tdm_offset += tdm_bytes_written;

                if (tdm_offset == tdm_size) {
                    result = ESP_OK;
                    break;
                }
                if (result != ESP_ERR_TIMEOUT || tdm_bytes_written == 0) {
                    /* 当前转换块不完整，播放任务会把本次会话作为错误停止。 */
                    if (result == ESP_OK) {
                        result = ESP_FAIL;
                    }
                    break;
                }
                /* 超时前已有进度，继续提交同一转换块的剩余物理槽。 */
            }
            if (result != ESP_OK) {
                break;
            }
            *bytes_written += source_size;
        }
    }
    xSemaphoreGive(s_tx_mutex);
    return result;
}

esp_err_t audio_i2s_read(void *data,
                         size_t data_size,
                         size_t *bytes_read,
                         uint32_t timeout_ms)
{
    if (data == NULL || data_size == 0 || bytes_read == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_control_mutex == NULL || s_rx_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_rx_mutex, portMAX_DELAY);
    xSemaphoreTake(s_control_mutex, portMAX_DELAY);
    esp_err_t result = ESP_ERR_INVALID_STATE;
    const bool can_read = s_rx_channel != NULL && s_rx_channel_enabled;
    xSemaphoreGive(s_control_mutex);
    if (can_read) {
        result = i2s_channel_read(s_rx_channel,
                                  data,
                                  data_size,
                                  bytes_read,
                                  timeout_ms);
    }
    xSemaphoreGive(s_rx_mutex);
    return result;
}
