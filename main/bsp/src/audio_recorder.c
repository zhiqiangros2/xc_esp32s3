/**
 * @file audio_recorder.c
 * @brief BSP 层 ES7210 到 SD 卡的异步 PCM WAV 录音任务。
 *
 * 本模块是 I2S RX 的唯一读取者。audio_recorder_init() 启动 16 kHz RX 后，
 * recorder_task 会在整个系统运行期间持续读取四槽 TDM DMA；没有开始保存
 * WAV 时也不能停止读取，否则 AEC、WakeNet 和 MultiNet 将失去输入。
 *
 * ES7210 每个 16 bit TDM 帧的物理槽顺序是：
 *
 *   [slot0=MIC1, slot1=MIC3, slot2=MIC2, slot3=MIC4]
 *
 * 同一块 DMA 数据在 split_tdm_frames() 中分成两条彼此独立的数据通路：
 *
 * 1. 常驻语音识别通路：始终提取 slot0+slot1，即 16 kHz 的 MIC1+MIC3。
 *    MIC1 是近端麦克风，MIC3 是 ES8311 播放参考；采集率已经等于 ESP-SR
 *    要求的 16 kHz，回调可直接交给 AFE，不再执行 FIR 和降采样。
 * 2. WAV 保存通路：仅在收到 START 并成功创建文件后提取 slot0+slot2，
 *    按 [MIC1,MIC2] 写成 16 kHz、16 bit、双声道 PCM WAV。该通路保存原始
 *    采样，不使用 MIC3，也不写入 AFE 处理后的数据。
 *
 * START/STOP 只创建或结束 WAV 文件，不启停常驻 RX。TX 和 RX 使用独立 DMA
 * 及锁，所以 16 kHz 播放、WAV 录音和常驻唤醒可以同时工作。LVGL 回调只
 * 发送命令，不直接执行 I2S 或文件操作。
 */
#include "audio_recorder.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "audio_i2s.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sd_fatfs.h"

#define RECORDER_DIRECTORY SD_MOUNT_POINT "/music"
#define RECORDER_SAMPLE_RATE_HZ AUDIO_I2S_SAMPLE_RATE_HZ
/*
 * WAV 固定保存未经 AFE 处理的 16 kHz 原始双声道：
 *   左声道 = ES7210 MIC1；右声道 = ES7210 MIC2。
 * 当前硬件 MIC2 接地，因此右声道应接近静音；仍保留它是为了维持既定 WAV
 * 文件格式。MIC3 只进入 AEC 参考通路，绝不会被写成 WAV 右声道。
 */
#define RECORDER_CHANNEL_COUNT 2U
#define RECORDER_BITS_PER_SAMPLE 16U
#define RECORDER_BLOCK_ALIGN \
    (RECORDER_CHANNEL_COUNT * RECORDER_BITS_PER_SAMPLE / 8U)
#define RECORDER_BYTE_RATE (RECORDER_SAMPLE_RATE_HZ * RECORDER_BLOCK_ALIGN)
#define RECORDER_WAV_HEADER_SIZE 44U
/*
 * DMA 原始缓冲保存 16 KiB 四槽 TDM：
 *   每帧 = 4 槽 x 16 bit = 8 字节；
 *   16384 / 8 = 2048 帧；
 *   2048 / 16000 = 128 ms。
 *
 * ES7210 的 1xFS、Philips I2S TDM 线上顺序为
 * [MIC1, MIC3, MIC2, MIC4]。每个原始块拆成：
 *   WAV [MIC1,MIC2] = 2048 帧 x 4 字节 = 8192 字节；
 *   AEC MIC1        = 2048 帧 x 2 字节 = 4096 字节；
 *   AEC MIC3        = 2048 帧 x 2 字节 = 4096 字节。
 */
#define RECORDER_TDM_SLOT_COUNT 4U
#define RECORDER_TDM_FRAME_SIZE \
    (RECORDER_TDM_SLOT_COUNT * RECORDER_BITS_PER_SAMPLE / 8U)
#define RECORDER_TDM_BUFFER_SIZE (16U * 1024U)
#define RECORDER_BUFFER_FRAME_COUNT \
    (RECORDER_TDM_BUFFER_SIZE / RECORDER_TDM_FRAME_SIZE)
#define RECORDER_WAV_BUFFER_SIZE \
    (RECORDER_BUFFER_FRAME_COUNT * RECORDER_BLOCK_ALIGN)
#define RECORDER_AEC_CHANNEL_BUFFER_SIZE \
    (RECORDER_BUFFER_FRAME_COUNT * sizeof(int16_t))
#define RECORDER_I2S_READ_TIMEOUT_MS 100U
#define RECORDER_TASK_STACK_SIZE 6144U
#define RECORDER_TASK_PRIORITY 5U
#define RECORDER_COMMAND_QUEUE_LENGTH 4U
#define RECORDER_FILE_NUMBER_MAX 99999U

typedef enum {
    RECORDER_COMMAND_START,
    RECORDER_COMMAND_STOP,
} recorder_command_t;

static QueueHandle_t s_command_queue;
static TaskHandle_t s_recorder_task;
/* I2S DMA 原始四槽数据，以及拆分后的 WAV/AEC 工作缓冲。 */
static int16_t *s_tdm_buffer;
static int16_t *s_wav_buffer;
static int16_t *s_aec_mic1_buffer;
static int16_t *s_aec_mic3_buffer;
static bool s_initialized;
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_aec_callback_lock = portMUX_INITIALIZER_UNLOCKED;
static audio_recorder_aec_input_callback_t s_aec_callback;
static void *s_aec_user_context;
static audio_recorder_status_t s_status = {
    .state = AUDIO_RECORDER_IDLE,
    .last_error = ESP_OK,
};
static const char *TAG = "RECORDER";

static void put_le16(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)(value & 0xFFU);
    destination[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static void put_le32(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value & 0xFFU);
    destination[1] = (uint8_t)((value >> 8) & 0xFFU);
    destination[2] = (uint8_t)((value >> 16) & 0xFFU);
    destination[3] = (uint8_t)((value >> 24) & 0xFFU);
}

/**
 * @brief 把一段数据完整写入文件描述符。
 *
 * POSIX write() 允许短写，因此循环到全部数据写完。即使中途失败也返回已经
 * 写入的字节数，使录音错误路径能够用实际长度回填 WAV 头。该接口不创建
 * Newlib FILE，也不会在录音开始时动态申请 stdio 递归锁。
 */
static esp_err_t write_file_all(int file_descriptor,
                                const void *buffer,
                                size_t size,
                                size_t *bytes_written)
{
    const uint8_t *source = buffer;
    size_t total_written = 0;

    while (total_written < size) {
        const ssize_t write_result = write(file_descriptor,
                                           source + total_written,
                                           size - total_written);
        if (write_result > 0) {
            total_written += (size_t)write_result;
            continue;
        }
        if (write_result < 0 && errno == EINTR) {
            continue;
        }
        if (bytes_written != NULL) {
            *bytes_written = total_written;
        }
        return ESP_FAIL;
    }

    if (bytes_written != NULL) {
        *bytes_written = total_written;
    }
    return ESP_OK;
}

/** 写入标准 PCM WAV 头；停止时以最终 data_size 再调用一次即可修正长度。 */
static esp_err_t write_wav_header(int file_descriptor, uint32_t data_size)
{
    uint8_t header[RECORDER_WAV_HEADER_SIZE] = {0};
    memcpy(&header[0], "RIFF", 4);
    put_le32(&header[4], data_size + 36U);
    memcpy(&header[8], "WAVE", 4);
    memcpy(&header[12], "fmt ", 4);
    put_le32(&header[16], 16U);
    put_le16(&header[20], 1U);
    put_le16(&header[22], RECORDER_CHANNEL_COUNT);
    put_le32(&header[24], RECORDER_SAMPLE_RATE_HZ);
    put_le32(&header[28], RECORDER_BYTE_RATE);
    put_le16(&header[32], RECORDER_BLOCK_ALIGN);
    put_le16(&header[34], RECORDER_BITS_PER_SAMPLE);
    memcpy(&header[36], "data", 4);
    put_le32(&header[40], data_size);

    if (lseek(file_descriptor, 0, SEEK_SET) == (off_t)-1 ||
        write_file_all(file_descriptor,
                       header,
                       sizeof(header),
                       NULL) != ESP_OK ||
        fsync(file_descriptor) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void set_status(audio_recorder_state_t state,
                       esp_err_t error,
                       uint32_t elapsed_seconds,
                       const char *file_name)
{
    audio_recorder_status_t status = {
        .state = state,
        .last_error = error,
        .elapsed_seconds = elapsed_seconds,
    };
    if (file_name != NULL) {
        snprintf(status.file_name,
                 sizeof(status.file_name),
                 "%s",
                 file_name);
    }

    /* 先在栈上构造完整快照，临界区内只赋值，缩短关中断时间。 */
    portENTER_CRITICAL(&s_status_lock);
    s_status = status;
    portEXIT_CRITICAL(&s_status_lock);
}

static void update_elapsed_time(uint32_t data_size)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.elapsed_seconds = data_size / RECORDER_BYTE_RATE;
    portEXIT_CRITICAL(&s_status_lock);
}

/** 优先从 PSRAM 分配大块音频缓存，PSRAM 不可用时退回内部 8-bit RAM。 */
static void *allocate_audio_buffer(size_t size)
{
    void *buffer = heap_caps_malloc(size,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        buffer = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return buffer;
}

/** 释放初始化阶段已取得的全部工作缓冲，供任一失败分支统一清理。 */
static void free_audio_buffers(void)
{
    heap_caps_free(s_aec_mic3_buffer);
    heap_caps_free(s_aec_mic1_buffer);
    heap_caps_free(s_wav_buffer);
    heap_caps_free(s_tdm_buffer);
    s_aec_mic3_buffer = NULL;
    s_aec_mic1_buffer = NULL;
    s_wav_buffer = NULL;
    s_tdm_buffer = NULL;
}

/**
 * 把 ES7210 四槽原始帧拆成两个互不混用的数据通路。
 *
 * 数据手册 1xFS I2S-TDM 时序规定一个 LRCK 周期依次为
 * [MIC1,MIC3,MIC2,MIC4]，所以：
 * - WAV 左/右声道取槽 0/2，保持用户需要的 MIC1+MIC2 文件；
 * - AEC 近端/参考取槽 0/1，得到同一采样时刻的 MIC1+MIC3。
 *
 * 两条通路在同一次遍历中生成，但生命周期不同：AEC 回调只要已注册就会在
 * 每个 DMA 块执行，与 file_descriptor 是否有效无关；s_wav_buffer 虽然每块
 * 都会填充，但只有 file_descriptor >= 0 时才写入 SD 卡。因此停止 WAV 录音
 * 不会停止语音识别，语音识别中的 AFE 也不会改变 WAV 文件内容。
 *
 * @param frame_count 本次完整 TDM 帧数，不得超过工作缓冲容量。
 * @return 拆分后可写入 WAV 的字节数。
 */
static size_t split_tdm_frames(size_t frame_count)
{
    audio_recorder_aec_input_callback_t callback;
    void *user_context;
    portENTER_CRITICAL(&s_aec_callback_lock);
    callback = s_aec_callback;
    user_context = s_aec_user_context;
    portEXIT_CRITICAL(&s_aec_callback_lock);

    for (size_t frame = 0; frame < frame_count; ++frame) {
        const int16_t mic1 = s_tdm_buffer[frame * 4U];
        const int16_t mic3 = s_tdm_buffer[frame * 4U + 1U];
        const int16_t mic2 = s_tdm_buffer[frame * 4U + 2U];

        s_wav_buffer[frame * 2U] = mic1;
        s_wav_buffer[frame * 2U + 1U] = mic2;
        if (callback != NULL) {
            s_aec_mic1_buffer[frame] = mic1;
            s_aec_mic3_buffer[frame] = mic3;
        }
    }

    if (callback != NULL) {
        callback(s_aec_mic1_buffer,
                 s_aec_mic3_buffer,
                 frame_count,
                 user_context);
    }
    return frame_count * RECORDER_BLOCK_ALIGN;
}

static esp_err_t ensure_recording_directory(void)
{
    struct stat information;
    if (stat(RECORDER_DIRECTORY, &information) == 0) {
        return S_ISDIR(information.st_mode) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (errno != ENOENT) {
        return ESP_FAIL;
    }
    if (mkdir(RECORDER_DIRECTORY, 0775) != 0 && errno != EEXIST) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

/** 选择第一个未占用的 REC00001.wav～REC99999.wav，避免覆盖已有录音。 */
static esp_err_t choose_recording_path(char *path,
                                       size_t path_size,
                                       char *file_name,
                                       size_t file_name_size)
{
    struct stat information;
    for (unsigned int number = 1;
         number <= RECORDER_FILE_NUMBER_MAX;
         ++number) {
        snprintf(file_name, file_name_size, "REC%05u.wav", number);
        const int length = snprintf(path,
                                    path_size,
                                    "%s/%s",
                                    RECORDER_DIRECTORY,
                                    file_name);
        if (length < 0 || (size_t)length >= path_size) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (stat(path, &information) != 0) {
            return errno == ENOENT ? ESP_OK : ESP_FAIL;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t start_session(int *file_descriptor,
                               char *path,
                               size_t path_size,
                               char *file_name,
                               size_t file_name_size)
{
    esp_err_t result = ensure_recording_directory();
    if (result != ESP_OK) {
        ESP_LOGE(TAG,
                 "Cannot create/open %s: errno=%d (%s)",
                 RECORDER_DIRECTORY,
                 errno,
                 strerror(errno));
        return result;
    }
    result = choose_recording_path(path,
                                   path_size,
                                   file_name,
                                   file_name_size);
    if (result != ESP_OK) {
        return result;
    }

    *file_descriptor = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (*file_descriptor < 0) {
        ESP_LOGE(TAG,
                 "Cannot create %s: errno=%d (%s)",
                 path,
                 errno,
                 strerror(errno));
        return ESP_FAIL;
    }
    result = write_wav_header(*file_descriptor, 0);
    if (result != ESP_OK) {
        close(*file_descriptor);
        *file_descriptor = -1;
        unlink(path);
        return result;
    }

    /* 文件名只在 WAV 成功结束并关闭后通过 SAVED 状态发布给界面。 */
    set_status(AUDIO_RECORDER_RECORDING, ESP_OK, 0, "");
    ESP_LOGI(TAG, "Recording started: %s", path);
    return ESP_OK;
}

/** 回填文件头并关闭文件；常驻 I2S RX 不受 WAV 会话启停影响。 */
static esp_err_t finish_session(int *file_descriptor,
                                uint32_t data_size,
                                const char *path,
                                const char *file_name)
{
    esp_err_t result = write_wav_header(*file_descriptor, data_size);
    if (close(*file_descriptor) != 0 && result == ESP_OK) {
        result = ESP_FAIL;
    }
    *file_descriptor = -1;

    const uint32_t elapsed_seconds = data_size / RECORDER_BYTE_RATE;
    if (result == ESP_OK) {
        set_status(AUDIO_RECORDER_SAVED,
                   ESP_OK,
                   elapsed_seconds,
                   file_name);
        ESP_LOGI(TAG,
                 "Recording saved: %s, %" PRIu32 " bytes",
                 path,
                 data_size);
    } else {
        set_status(AUDIO_RECORDER_ERROR,
                   result,
                   elapsed_seconds,
                   "");
        ESP_LOGE(TAG,
                 "Recording finalization failed for %s: %s",
                 path,
                 esp_err_to_name(result));
    }
    return result;
}

static void recorder_task(void *argument)
{
    (void)argument;
    int file_descriptor = -1;
    uint32_t data_size = 0;
    char path[128] = {0};
    char file_name[AUDIO_RECORDER_FILE_NAME_MAX] = {0};

    for (;;) {
        recorder_command_t command;
        /*
         * 命令检查不阻塞：即使当前没有 WAV 文件，也必须继续排空 RX DMA 并
         * 把原始 16 kHz MIC1+MIC3 投递给常驻语音识别。一次循环最多处理一个
         * 命令，随后立即回到采集；DMA 的 100 ms 等待时间同时限制 START/STOP
         * 延迟。START/STOP 只改变 file_descriptor，不改变 RX 和识别回调状态。
         */
        if (xQueueReceive(s_command_queue, &command, 0) == pdTRUE) {
            if (command == RECORDER_COMMAND_START && file_descriptor < 0) {
                data_size = 0;
                path[0] = '\0';
                file_name[0] = '\0';
                const esp_err_t result = start_session(&file_descriptor,
                                                        path,
                                                        sizeof(path),
                                                        file_name,
                                                        sizeof(file_name));
                if (result != ESP_OK) {
                    set_status(AUDIO_RECORDER_ERROR,
                               result,
                               0,
                               "");
                    ESP_LOGE(TAG,
                             "Cannot start recording: %s",
                             esp_err_to_name(result));
                }
            } else if (command == RECORDER_COMMAND_STOP &&
                       file_descriptor >= 0) {
                finish_session(&file_descriptor,
                               data_size,
                               path,
                               file_name);
            }
        }

        size_t bytes_read = 0;
        esp_err_t result = audio_i2s_read(s_tdm_buffer,
                                          RECORDER_TDM_BUFFER_SIZE,
                                          &bytes_read,
                                          RECORDER_I2S_READ_TIMEOUT_MS);
        if (bytes_read > 0) {
            if (bytes_read % RECORDER_TDM_FRAME_SIZE != 0) {
                result = ESP_ERR_INVALID_SIZE;
            } else {
                const size_t frame_count =
                    bytes_read / RECORDER_TDM_FRAME_SIZE;
                const size_t wav_bytes = split_tdm_frames(frame_count);
                if (file_descriptor >= 0) {
                    const uint32_t maximum_data_size = UINT32_MAX - 36U;
                    if (wav_bytes > maximum_data_size - data_size) {
                        result = ESP_ERR_INVALID_SIZE;
                    } else {
                        size_t bytes_written = 0;
                        const esp_err_t write_result =
                            write_file_all(file_descriptor,
                                           s_wav_buffer,
                                           wav_bytes,
                                           &bytes_written);
                        data_size += (uint32_t)bytes_written;
                        update_elapsed_time(data_size);
                        if (write_result != ESP_OK) {
                            result = write_result;
                        }
                    }
                }
            }
        }

        if (result != ESP_OK && result != ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG,
                     "Recording I/O failed: %s",
                     esp_err_to_name(result));
            if (file_descriptor >= 0) {
                write_wav_header(file_descriptor, data_size);
                close(file_descriptor);
                file_descriptor = -1;
                set_status(AUDIO_RECORDER_ERROR,
                           result,
                           data_size / RECORDER_BYTE_RATE,
                           "");
            }

            /*
             * 非超时错误可能表示 RX 通道进入异常状态。停止并按固定 16 kHz
             * 重启，恢复常驻唤醒；失败时退避 1 秒，避免错误日志空转刷屏。
             */
            audio_i2s_record_stop();
            vTaskDelay(pdMS_TO_TICKS(20));
            const esp_err_t restart_result =
                audio_i2s_record_start(RECORDER_SAMPLE_RATE_HZ);
            if (restart_result != ESP_OK) {
                ESP_LOGE(TAG,
                         "Cannot restart always-on capture: %s",
                         esp_err_to_name(restart_result));
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
    }
}

esp_err_t audio_recorder_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    /* ES7210 由板级启动流程独立初始化；这里只创建录音软件资源。 */
    s_tdm_buffer = allocate_audio_buffer(RECORDER_TDM_BUFFER_SIZE);
    s_wav_buffer = allocate_audio_buffer(RECORDER_WAV_BUFFER_SIZE);
    s_aec_mic1_buffer =
        allocate_audio_buffer(RECORDER_AEC_CHANNEL_BUFFER_SIZE);
    s_aec_mic3_buffer =
        allocate_audio_buffer(RECORDER_AEC_CHANNEL_BUFFER_SIZE);
    if (s_tdm_buffer == NULL || s_wav_buffer == NULL ||
        s_aec_mic1_buffer == NULL || s_aec_mic3_buffer == NULL) {
        free_audio_buffers();
        return ESP_ERR_NO_MEM;
    }

    s_command_queue = xQueueCreate(RECORDER_COMMAND_QUEUE_LENGTH,
                                   sizeof(recorder_command_t));
    if (s_command_queue == NULL) {
        free_audio_buffers();
        return ESP_ERR_NO_MEM;
    }

    /*
     * RX 在创建任务前启动，因此 audio_recorder_init() 返回 ESP_OK 时即可保证
     * 常驻采集已经可用。以后 START/STOP 只改变 WAV 文件状态；16 kHz 公共
     * 时钟始终保持，播放端只能同时播放同采样率文件。
     */
    esp_err_t result = audio_i2s_record_start(RECORDER_SAMPLE_RATE_HZ);
    if (result != ESP_OK) {
        vQueueDelete(s_command_queue);
        s_command_queue = NULL;
        free_audio_buffers();
        return result;
    }

    if (xTaskCreate(recorder_task,
                    "audio_recorder",
                    RECORDER_TASK_STACK_SIZE,
                    NULL,
                    RECORDER_TASK_PRIORITY,
                    &s_recorder_task) != pdPASS) {
        vQueueDelete(s_command_queue);
        s_command_queue = NULL;
        audio_i2s_record_stop();
        free_audio_buffers();
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    set_status(AUDIO_RECORDER_IDLE, ESP_OK, 0, "");
    return ESP_OK;
}

esp_err_t audio_recorder_start(void)
{
    if (!s_initialized || s_command_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    audio_recorder_status_t status;
    audio_recorder_get_status(&status);
    if (status.state == AUDIO_RECORDER_STARTING ||
        status.state == AUDIO_RECORDER_RECORDING ||
        status.state == AUDIO_RECORDER_STOPPING) {
        return ESP_ERR_INVALID_STATE;
    }

    set_status(AUDIO_RECORDER_STARTING, ESP_OK, 0, "");
    const recorder_command_t command = RECORDER_COMMAND_START;
    if (xQueueSendToBack(s_command_queue, &command, 0) != pdTRUE) {
        set_status(AUDIO_RECORDER_ERROR, ESP_ERR_TIMEOUT, 0, "");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t audio_recorder_stop(void)
{
    if (!s_initialized || s_command_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    audio_recorder_status_t status;
    audio_recorder_get_status(&status);
    if (status.state == AUDIO_RECORDER_IDLE ||
        status.state == AUDIO_RECORDER_SAVED ||
        status.state == AUDIO_RECORDER_ERROR) {
        return ESP_OK;
    }
    if (status.state == AUDIO_RECORDER_STOPPING) {
        return ESP_OK;
    }

    set_status(AUDIO_RECORDER_STOPPING,
               ESP_OK,
               status.elapsed_seconds,
               status.file_name);
    const recorder_command_t command = RECORDER_COMMAND_STOP;
    if (xQueueSendToBack(s_command_queue, &command, 0) != pdTRUE) {
        /* 队列满时没有真正发出 STOP，恢复原状态供界面下一周期重试。 */
        set_status(status.state,
                   status.last_error,
                   status.elapsed_seconds,
                   status.file_name);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t audio_recorder_get_status(audio_recorder_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_status_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_status_lock);
    return ESP_OK;
}

esp_err_t audio_recorder_set_aec_input_callback(
    audio_recorder_aec_input_callback_t callback,
    void *user_context)
{
    /*
     * 回调和上下文必须作为一个快照更新，避免录音任务观察到新回调搭配旧
     * 上下文。callback=NULL 表示取消，此时上下文一并清空。
     */
    portENTER_CRITICAL(&s_aec_callback_lock);
    s_aec_callback = callback;
    s_aec_user_context = callback != NULL ? user_context : NULL;
    portEXIT_CRITICAL(&s_aec_callback_lock);
    return ESP_OK;
}
