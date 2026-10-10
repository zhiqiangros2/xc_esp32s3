/**
 * @file speech_recognition.c
 * @brief BSP 层 MIC1+MIC3 常驻 AEC、唤醒词和中文命令词识别。
 *
 * 当前硬件连接和数据方向：
 *
 *                                      ESP32-S3
 *                     +---------------------------------------+
 *   WAV 播放 ----------> I2S TX [L,0,R,0]                    |
 *                     |                  |                    |
 *                     |                  v                    |
 *                     |              ES8311 DAC ---> 扬声器   |
 *                     |                  |                    |
 *                     |                  | 模拟播放参考回采    |
 *                     |                  v                    |
 *   板载麦克风 ------->| ES7210 MIC1   ES7210 MIC3            |
 *                     |        \         /                    |
 *                     |   I2S RX 四槽 TDM                      |
 *                     | [MIC1,MIC3,MIC2,MIC4]                 |
 *                     +------------------+--------------------+
 *                                        |
 *                                        v
 *                         audio_recorder 采集任务
 *                         （唯一读取 I2S RX DMA）
 *
 * audio_recorder 读取一次 TDM 数据后分成两条互不影响的支路：
 *
 *                    +----------------------+----------------------+
 *                    |                                             |
 *                    v                                             v
 *        常驻语音识别支路                                按需 WAV 录音支路
 *        MIC1 + MIC3                                    MIC1 + MIC2
 *        [近端语音,播放参考]                            [左声道,右声道]
 *                    |                                             |
 *                    v                                             v
 *        capture_callback() 非阻塞                    START 后写入 SD /music
 *                    |                                STOP 后补 WAV 头并关文件
 *                    v
 *        PSRAM StreamBuffer（约 2.05 秒）
 *                    |
 *                    v
 *        speech_feed_task（CPU0）
 *        拼满 AFE 一块输入并调用 feed()
 *                    |
 *                    v
 *        ESP-SR AFE 输入 [M,R]
 *        AEC -> NS -> VAD -> AGC -> WakeNet
 *                    |
 *                    v
 *        speech_detect_task（CPU1）调用 fetch()
 *        等待“你好小鑫” -> MultiNet 命令窗口
 *                    |
 *                    v
 *        长度为 1 的动作队列（只保留最新灯状态）
 *                    |
 *                    v
 *        speech_action_task -> AW9523B -> 红灯开/关
 *
 * M 是 MIC1 近端人声，R 是 MIC3 从 ES8311 模拟输出取得的播放参考。AFE
 * 依靠 R 做 AEC，之后启用 NS、VAD、AGC 和 WakeNet；AFE fetch 输出增强后的
 * 16 kHz 单声道语音及唤醒状态。检测到“你好小鑫”后，这份单声道数据才送入
 * MultiNet 识别“打开红灯/关闭红灯”。
 *
 * audio_recorder 的回调只执行一次内存交错复制并立即返回；AFE 和 MultiNet
 * 都在独立任务中运行，不阻塞 RX DMA。WAV 分支始终保存 MIC1+MIC2 原始
 * 采样，不会改成 MIC1+MIC3，也不会保存 AFE 输出。
 */
#include "speech_recognition.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "audio_i2s.h"
#include "audio_recorder.h"
#include "aw9523b.h"
#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_wn_models.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "model_path.h"

/* ESP-SR AFE 固定接收 16 kHz PCM；输入 M 和 R 共两个交错通道。 */
#define SPEECH_AFE_SAMPLE_RATE_HZ 16000U
#define SPEECH_INPUT_CHANNEL_COUNT 2U
/* 编译阶段就阻止 I2S 采样率和 AFE 采样率不一致的固件生成。 */
_Static_assert(AUDIO_I2S_SAMPLE_RATE_HZ == SPEECH_AFE_SAMPLE_RATE_HZ,
               "I2S capture rate must match the ESP-SR AFE rate");

/*
 * audio_recorder 每个 16 KiB TDM 块包含 2048 帧。回调工作缓冲恰好容纳
 * 2048 组 [MIC1,MIC3]：
 *
 *   2048 帧 x 2 通道 x 2 字节/样本 = 8192 字节
 *   2048 帧 / 16000 帧/秒 = 128 ms 音频
 *
 * 如果回调一次交来的样本更多，capture_callback() 会按 2048 帧拆块。
 */
#define SPEECH_CALLBACK_FRAME_CAPACITY 2048U
#define SPEECH_CALLBACK_BUFFER_SIZE \
    (SPEECH_CALLBACK_FRAME_CAPACITY * SPEECH_INPUT_CHANNEL_COUNT * \
     sizeof(int16_t))
/*
 * 16 kHz 双通道流速为 16000 x 2 x 2 = 64000 字节/秒。128 KiB 流缓冲
 * 可保存约 2.05 秒输入，用来吸收 LVGL、SD 卡和模型任务的短时调度抖动。
 * 缓冲满时丢弃完整输入块，绝不写入半帧破坏后续通道对齐。
 */
#define SPEECH_STREAM_BUFFER_SIZE (128U * 1024U)

/*
 * 三个常驻任务的栈大小和优先级。ESP32-S3 的 ESP-IDF 任务栈单位是字节。
 * Feed 与 Detect 都是持续音频处理，使用相同优先级 5；Action 只执行低频
 * I2C 灯控，优先级 4 即可。初始化时 Feed 固定在 CPU0，Detect 固定在 CPU1，
 * 避免 AFE feed/fetch 的持续计算集中到同一核心；Action 由调度器选择核心。
 */
#define SPEECH_FEED_TASK_STACK_SIZE 6144U
#define SPEECH_DETECT_TASK_STACK_SIZE 8192U
#define SPEECH_ACTION_TASK_STACK_SIZE 3072U
#define SPEECH_FEED_TASK_PRIORITY 5U
#define SPEECH_DETECT_TASK_PRIORITY 5U
#define SPEECH_ACTION_TASK_PRIORITY 4U
/* 唤醒后允许用户说完一条 MultiNet 命令的最长时间。 */
#define SPEECH_COMMAND_TIMEOUT_MS 6000
/* 缓冲溢出日志限频，避免实时音频异常时连续打印进一步拖慢系统。 */
#define SPEECH_DROP_LOG_INTERVAL_MS 5000U

/* MultiNet 返回的命令编号；编号必须大于 0，且同一个动作保持唯一。 */
#define SPEECH_COMMAND_RED_ON 1
#define SPEECH_COMMAND_RED_OFF 2

/* 识别任务投递给硬件动作任务的内部消息，不等同于 MultiNet 命令编号。 */
typedef enum {
    SPEECH_ACTION_RED_ON,
    SPEECH_ACTION_RED_OFF,
} speech_action_t;

/* ESP-SR 模型列表以及由模型配置创建的 AFE、MultiNet 实例。 */
static srmodel_list_t *s_models;
static const esp_afe_sr_iface_t *s_afe_handle;
static esp_afe_sr_data_t *s_afe_data;
static esp_mn_iface_t *s_multinet;
static model_iface_data_t *s_multinet_data;

/*
 * 采集回调到 Feed 任务之间的数据通道：控制结构在静态内存中，128 KiB 数据区
 * 在初始化时分配到 PSRAM。回调交错缓冲也在 PSRAM；AFE 当前输入块位于内部
 * RAM，保证 AFE 处理时访问稳定且不受 PSRAM 带宽影响。
 */
static StreamBufferHandle_t s_capture_stream;
static StaticStreamBuffer_t s_capture_stream_control;
static uint8_t *s_capture_stream_storage;
static int16_t *s_callback_interleaved;
static int16_t *s_afe_input;
static size_t s_afe_feed_samples;

/* 任务、灯控消息队列及跨回调/任务共享的运行状态。 */
static QueueHandle_t s_action_queue;
static TaskHandle_t s_feed_task;
static TaskHandle_t s_detect_task;
static TaskHandle_t s_action_task;
/* capture_callback() 原子累加，speech_feed_task() 原子取走并清零。 */
static volatile uint32_t s_dropped_capture_blocks;
/* 下面三个标志只用于初始化失败时判断哪些资源已经取得。 */
static bool s_commands_allocated;
static bool s_callback_registered;
static bool s_initialized;
static const char *TAG = "SPEECH";

/**
 * 输出语音初始化各阶段的堆状态。
 *
 * internal free 表示当前还能用于任务栈、I2S DMA 和驱动控制对象的片内
 * 8-bit RAM；largest 表示其中最大的连续块，能比总空闲量更早暴露碎片问题；
 * minimum 是启动以来片内堆的历史最低余量。PSRAM 只记录当前空闲量和最大
 * 连续块，用于确认 AFE、MultiNet 及应用大缓冲确实优先分配到了外部内存。
 */
static void log_heap_state(const char *stage)
{
    const uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

    ESP_LOGI(TAG,
             "%s heap: internal free/largest/min=%u/%u/%u, "
             "PSRAM free/largest=%u/%u",
             stage,
             (unsigned)heap_caps_get_free_size(internal_caps),
             (unsigned)heap_caps_get_largest_free_block(internal_caps),
             (unsigned)heap_caps_get_minimum_free_size(internal_caps),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

/**
 * audio_recorder 的非阻塞回调。
 *
 * 输入是同一批 TDM 帧拆出的两个平面数组。这里重新交错为 AFE 所需的
 * [MIC1,MIC3]，并以完整块写入单写者/单读者 StreamBuffer。缓冲不足时整块
 * 丢弃并累计计数，不能等待，否则 RX DMA 会因 AFE 暂时繁忙而溢出。
 *
 * 输入：
 *   mic1_samples = [M0,M1,M2,...]，板载麦克风的近端语音
 *   mic3_samples = [R0,R1,R2,...]，ES8311 播放声音的模拟参考
 *
 * 输出：
 *   s_callback_interleaved = [M0,R0,M1,R1,M2,R2,...]
 *
 * 该函数运行在 audio_recorder 的 I2S 采集任务上下文，不是硬件 ISR，但它仍
 * 必须尽快返回，让采集任务继续读取 RX DMA。因此发送超时固定为 0，不允许
 * 在这里等待 Feed 任务腾出空间。
 */
static void capture_callback(const int16_t *mic1_samples,
                             const int16_t *mic3_samples,
                             size_t sample_count,
                             void *user_context)
{
    (void)user_context;
    /* 已经处理到输入平面数组的第几个样本。 */
    size_t source_offset = 0;

    while (source_offset < sample_count) {
        /* 单次只处理工作缓冲能够容纳的帧数。 */
        size_t frame_count = sample_count - source_offset;
        if (frame_count > SPEECH_CALLBACK_FRAME_CAPACITY) {
            frame_count = SPEECH_CALLBACK_FRAME_CAPACITY;
        }

        /* 把两个平面数组转换成 AFE 要求的 M、R 交错数组。 */
        for (size_t frame = 0; frame < frame_count; ++frame) {
            s_callback_interleaved[frame * 2U] =
                mic1_samples[source_offset + frame];
            s_callback_interleaved[frame * 2U + 1U] =
                mic3_samples[source_offset + frame];
        }

        const size_t block_size =
            frame_count * SPEECH_INPUT_CHANNEL_COUNT * sizeof(int16_t);

        /*
         * 先确认整块数据能够放下。空间不够就整块丢弃，不能只写一部分，
         * 否则后续的 M、R 交错顺序会错位。
         */
        const size_t available_size =
            xStreamBufferSpacesAvailable(s_capture_stream);
        if (available_size < block_size) {
            __atomic_fetch_add(&s_dropped_capture_blocks,
                               1U,
                               __ATOMIC_RELAXED);
            source_offset += frame_count;
            continue;
        }

        /* 超时为 0：只使用当前可用空间，不阻塞 I2S 采集任务。 */
        const size_t sent_size = xStreamBufferSend(s_capture_stream,
                                                   s_callback_interleaved,
                                                   block_size,
                                                   0);
        if (sent_size != block_size) {
            /* 单写者模式下通常不会发生；保留计数用于发现异常。 */
            __atomic_fetch_add(&s_dropped_capture_blocks,
                               1U,
                               __ATOMIC_RELAXED);
        }
        source_offset += frame_count;
    }
}

/**
 * 把一帧原生 16 kHz [MIC1,MIC3] 直接送入 AFE。
 *
 * capture_callback() 已经把两个平面通道交错为 [M0,R0,M1,R1,...]。I2S、
 * ES7210 和 AFE 都使用 16 kHz，因此这里只按 AFE feed chunk 大小读取完整块；
 * 不做 FIR、不抽点，也不创建额外的中间复制缓冲。
 *
 *   StreamBuffer --阻塞读取--> s_afe_input --feed()--> AFE 内部输入队列
 *
 * AFE 的块大小由所加载模型决定，不能假定等于 audio_recorder 的 2048 帧。
 * xStreamBufferReceive() 也允许提前返回当前已有字节，所以 received_size 放在
 * for 循环外保存进度；只有凑齐 afe_block_size 才调用 feed()。
 */
static void speech_feed_task(void *argument)
{
    (void)argument;
    const size_t afe_block_size = s_afe_feed_samples *
                                  SPEECH_INPUT_CHANNEL_COUNT *
                                  sizeof(int16_t);
    const TickType_t drop_report_interval =
        pdMS_TO_TICKS(SPEECH_DROP_LOG_INTERVAL_MS);
    size_t received_size = 0;
    uint32_t unreported_drop_count = 0;
    TickType_t last_drop_report_time = xTaskGetTickCount();

    for (;;) {
        /* 1. 每次读取现有数据；不足一块就保存进度并继续等待。 */
        received_size += xStreamBufferReceive(
            s_capture_stream,
            (uint8_t *)s_afe_input + received_size,
            afe_block_size - received_size,
            portMAX_DELAY);
        if (received_size < afe_block_size) {
            continue;
        }
        received_size = 0;

        /* 2. 把完整的 [MIC1,MIC3] 数据块交给 AFE 处理。 */
        if (s_afe_handle->feed(s_afe_data, s_afe_input) < 0) {
            ESP_LOGE(TAG, "AFE feed failed");
        }

        /* 3. 取走回调累计的丢块数，日志最多每 5 秒输出一次。 */
        unreported_drop_count += __atomic_exchange_n(
            &s_dropped_capture_blocks, 0U, __ATOMIC_RELAXED);
        if (unreported_drop_count == 0) {
            continue;
        }

        const TickType_t current_time = xTaskGetTickCount();
        if (current_time - last_drop_report_time < drop_report_interval) {
            continue;
        }

        ESP_LOGW(TAG,
                 "Capture stream full, dropped %" PRIu32 " block(s)",
                 unreported_drop_count);
        unreported_drop_count = 0;
        last_drop_report_time = current_time;
    }
}

/**
 * 串行执行 AW9523B 红灯控制。
 *
 * Detect 任务只向长度为 1 的队列写入目标状态，不直接访问 I2C。该任务永久
 * 阻塞等待动作，到达后再操作 AW9523B。这样模型识别不会被 I2C 事务阻塞，
 * 连续到达的新状态则由 xQueueOverwrite() 保留最新一个。
 */
static void speech_action_task(void *argument)
{
    (void)argument;
    for (;;) {
        speech_action_t action;
        xQueueReceive(s_action_queue, &action, portMAX_DELAY);

        esp_err_t result;
        const char *state_text;
        switch (action) {
            case SPEECH_ACTION_RED_ON:
                result =
                    aw9523b_set_box3_led(AW9523B_BOX3_LED_RED, true);
                state_text = "on";
                break;

            case SPEECH_ACTION_RED_OFF:
                result =
                    aw9523b_set_box3_led(AW9523B_BOX3_LED_RED, false);
                state_text = "off";
                break;

            default:
                ESP_LOGW(TAG, "Unknown speech action: %d", (int)action);
                continue;
        }

        if (result == ESP_OK) {
            ESP_LOGI(TAG, "Red LED %s", state_text);
        } else {
            ESP_LOGE(TAG,
                     "Cannot set red LED: %s",
                     esp_err_to_name(result));
        }
    }
}

/**
 * AFE fetch 同时给出增强后的单声道 PCM、VAD 和 WakeNet 状态。检测到
 * “你好小鑫”后关闭 WakeNet，打开 6 秒 MultiNet 命令窗口；识别或超时后
 * 清理 MultiNet 状态并重新启用 WakeNet，防止一句命令被重复执行。
 *
 * 状态流程：
 *
 *   WAIT_WAKE（waiting_for_command=false）
 *       |
 *       | fetch() 返回 WAKENET_DETECTED
 *       v
 *   WAIT_COMMAND（waiting_for_command=true，WakeNet 暂停）
 *       |                                      |
 *       | MultiNet 识别到命令                 | 6 秒超时
 *       v                                      v
 *   投递 RED_ON/RED_OFF                    不投递动作
 *       |                                      |
 *       +------------------+-------------------+
 *                          v
 *                清空 MultiNet 历史状态
 *                重新启用 WakeNet
 *                          |
 *                          v
 *                       WAIT_WAKE
 *
 * fetch() 会等待 AFE 输出，不进行空转轮询。WakeNet 已集成在 AFE 内部，
 * 唤醒结果直接由 result->wakeup_state 给出，不需要另建 WakeNet 任务。
 */
static void speech_detect_task(void *argument)
{
    (void)argument;
    bool waiting_for_command = false;

    for (;;) {
        /* 阻塞取得一块经过 AEC/NS/AGC 处理的单声道 PCM 及识别状态。 */
        afe_fetch_result_t *result = s_afe_handle->fetch(s_afe_data);
        if (result == NULL || result->ret_value == ESP_FAIL) {
            ESP_LOGE(TAG, "AFE fetch failed");
            continue;
        }

        if (result->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "Wake word detected: Ni Hao Xiao Xin");
            /* 新命令窗口从干净状态开始，窗口期间暂停再次唤醒。 */
            s_multinet->clean(s_multinet_data);
            s_afe_handle->disable_wakenet(s_afe_data);
            waiting_for_command = true;
        }
        if (!waiting_for_command) {
            continue;
        }

        const esp_mn_state_t state =
            s_multinet->detect(s_multinet_data, result->data);
        if (state == ESP_MN_STATE_DETECTED) {
            const esp_mn_results_t *commands =
                s_multinet->get_results(s_multinet_data);
            if (commands != NULL && commands->num > 0) {
                /* 结果按概率排序，当前只执行第一条最高概率命令。 */
                const int command_id = commands->command_id[0];
                ESP_LOGI(TAG,
                         "Command detected: id=%d, text=%s, probability=%.3f",
                         command_id,
                         commands->string,
                         commands->prob[0]);
                if (command_id == SPEECH_COMMAND_RED_ON) {
                    const speech_action_t action = SPEECH_ACTION_RED_ON;
                    /* 队列长度为 1：覆盖尚未执行的旧灯状态。 */
                    xQueueOverwrite(s_action_queue, &action);
                } else if (command_id == SPEECH_COMMAND_RED_OFF) {
                    const speech_action_t action = SPEECH_ACTION_RED_OFF;
                    xQueueOverwrite(s_action_queue, &action);
                }
            }
            waiting_for_command = false;
        } else if (state == ESP_MN_STATE_TIMEOUT) {
            ESP_LOGI(TAG, "Command window timed out");
            waiting_for_command = false;
        }

        if (!waiting_for_command) {
            /* 识别成功或超时都回到等待下一次唤醒的初始状态。 */
            s_multinet->clean(s_multinet_data);
            s_afe_handle->enable_wakenet(s_afe_data);
        }
    }
}

/**
 * 释放初始化失败前已经取得的资源；正常运行期没有反初始化入口。
 *
 * 释放顺序与初始化大致相反：
 *
 *   注销采集回调
 *       -> 删除三个任务
 *       -> 删除任务使用的队列
 *       -> 释放 MultiNet 命令表和实例
 *       -> 释放 AFE 实例和模型列表
 *       -> 删除 StreamBuffer
 *       -> 释放三个音频缓冲
 *
 * 必须先注销回调并停止任务，再释放它们访问的对象和内存，避免失败回滚期间
 * 出现任务继续读写已释放缓冲的竞态。
 */
static void cleanup_initialization(void)
{
    if (s_callback_registered) {
        audio_recorder_set_aec_input_callback(NULL, NULL);
        s_callback_registered = false;
    }
    if (s_action_task != NULL) {
        vTaskDelete(s_action_task);
        s_action_task = NULL;
    }
    if (s_detect_task != NULL) {
        vTaskDelete(s_detect_task);
        s_detect_task = NULL;
    }
    if (s_feed_task != NULL) {
        vTaskDelete(s_feed_task);
        s_feed_task = NULL;
    }
    if (s_action_queue != NULL) {
        vQueueDelete(s_action_queue);
        s_action_queue = NULL;
    }
    if (s_commands_allocated) {
        esp_mn_commands_free();
        s_commands_allocated = false;
    }
    if (s_multinet_data != NULL) {
        s_multinet->destroy(s_multinet_data);
        s_multinet_data = NULL;
    }
    s_multinet = NULL;
    if (s_afe_data != NULL) {
        s_afe_handle->destroy(s_afe_data);
        s_afe_data = NULL;
    }
    s_afe_handle = NULL;
    if (s_models != NULL) {
        esp_srmodel_deinit(s_models);
        s_models = NULL;
    }
    if (s_capture_stream != NULL) {
        vStreamBufferDelete(s_capture_stream);
        s_capture_stream = NULL;
    }
    heap_caps_free(s_afe_input);
    heap_caps_free(s_callback_interleaved);
    heap_caps_free(s_capture_stream_storage);
    s_afe_input = NULL;
    s_callback_interleaved = NULL;
    s_capture_stream_storage = NULL;
}

/**
 * 建立本应用自己的 MultiNet 命令表。
 *
 * MultiNet 命令使用不带声调的空格分隔拼音。命令编号随后由 Detect 任务映射
 * 到内部动作枚举：1 表示“打开红灯”，2 表示“关闭红灯”。commands_update()
 * 会检查拼音是否属于当前中文模型的可用音节，失败时打印具体短语。
 */
static esp_err_t configure_speech_commands(void)
{
    /* 命令表依附于当前 MultiNet 实例，先申请后逐条加入短语。 */
    esp_err_t result = esp_mn_commands_alloc(s_multinet,
                                              s_multinet_data);
    if (result != ESP_OK) {
        return result;
    }
    s_commands_allocated = true;

    result = esp_mn_commands_add(SPEECH_COMMAND_RED_ON,
                                 "da kai hong deng");
    if (result == ESP_OK) {
        result = esp_mn_commands_add(SPEECH_COMMAND_RED_OFF,
                                     "guan bi hong deng");
    }
    if (result != ESP_OK) {
        return result;
    }

    const esp_mn_error_t *errors = esp_mn_commands_update();
    if (errors != NULL) {
        for (int index = 0; index < errors->num; ++index) {
            ESP_LOGE(TAG,
                     "Unsupported command phrase: %s",
                     errors->phrases[index]->string);
        }
        return ESP_ERR_INVALID_ARG;
    }
    s_multinet->print_active_speech_commands(s_multinet_data);
    return ESP_OK;
}

/**
 * 初始化常驻语音识别通路。
 *
 * 初始化只允许成功一次；再次调用直接返回 ESP_OK。正常顺序如下：
 *
 *   1. 从 model 分区加载模型目录
 *   2. 选择“你好小鑫”WakeNet 和中文 MultiNet
 *   3. 创建 AFE，配置输入格式 MR、AEC/NS/VAD/AGC/WakeNet
 *   4. 查询并验证 AFE 的采样率、通道数和 feed 块大小
 *   5. 创建 MultiNet，验证其输入格式与 AFE fetch 输出一致
 *   6. 注册“打开红灯/关闭红灯”命令
 *   7. 分配 StreamBuffer、回调工作缓冲和 AFE 输入缓冲
 *   8. 创建动作队列以及 Feed、Detect、Action 三个任务
 *   9. 最后注册 audio_recorder 回调，开始接收常驻音频
 *
 * 第 9 步必须放在最后。若提前注册，I2S 采集任务可能在缓冲和消费任务尚未
 * 就绪时立即回调。任一步失败都跳到 failed，按 cleanup_initialization()
 * 的逆序规则回收已经创建的资源。
 *
 * @return ESP_OK 初始化成功或此前已经初始化。
 * @return ESP_ERR_NOT_FOUND model 分区或所需模型缺失。
 * @return ESP_ERR_NO_MEM 模型实例、任务、队列或缓冲分配失败。
 * @return ESP_ERR_INVALID_STATE AFE 与 I2S/MultiNet 的音频格式不匹配。
 */
esp_err_t speech_recognition_init(void)
{
    /* 幂等保护：避免重复创建模型实例、任务和采集回调。 */
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t result = ESP_FAIL;
    afe_config_t *afe_config = NULL;

    log_heap_state("Before ESP-SR");

    /*
     * 读取分区表中标签为 "model" 的 ESP-SR 模型分区，得到其中所有模型名。
     * 这里只加载目录；具体 AFE/MultiNet 实例在后续步骤创建。
     */
    s_models = esp_srmodel_init("model");
    if (s_models == NULL) {
        ESP_LOGE(TAG, "Cannot load ESP-SR model partition");
        result = ESP_ERR_NOT_FOUND;
        goto failed;
    }

    /* 从已打包模型中精确选择“你好小鑫”和中文 MultiNet。 */
    char *wakenet_model =
        esp_srmodel_filter(s_models, ESP_WN_PREFIX, "nihaoxiaoxin");
    char *multinet_model =
        esp_srmodel_filter(s_models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if (wakenet_model == NULL || multinet_model == NULL) {
        ESP_LOGE(TAG,
                 "Required models missing: WakeNet=%s, MultiNet=%s",
                 wakenet_model != NULL ? wakenet_model : "none",
                 multinet_model != NULL ? multinet_model : "none");
        result = ESP_ERR_NOT_FOUND;
        goto failed;
    }

    /*
     * 创建 AFE 配置。
     *
     * 输入格式 "MR" 明确告诉 AFE：每个 16 kHz 交错帧的第一通道 M 是近端
     * MIC1，第二通道 R 是 MIC3 播放参考。AEC 用 R 从 M 中抑制设备自身的
     * 播放声；随后由 VAD 判断是否存在语音，AGC 调整语音幅度，WakeNet 检测
     * 唤醒词。这里关闭 WebRTC NS：ESP-SR 会提示 NS 可能改变唤醒词的频谱
     * 特征并降低识别率。关闭 NS 不会关闭 AEC，MIC3 播放参考仍然有效。
     * fetch() 最终返回处理后的单声道 PCM 和唤醒状态。MultiNet 不属于 AFE，
     * 只有唤醒后才消费 fetch() 的单声道结果。
     *
     * 更多模型内存放入 PSRAM，保留内部 RAM 给 I2S DMA、任务栈和 AFE 输入。
     */
    afe_config = afe_config_init("MR",
                                 s_models,
                                 AFE_TYPE_SR,
                                 AFE_MODE_LOW_COST);
    if (afe_config == NULL) {
        result = ESP_ERR_NO_MEM;
        goto failed;
    }
    afe_config->aec_init = true;     /* 使用 MIC3 参考信号消除播放回声。 */
    afe_config->se_init = false;     /* 不启用独立的语音增强处理级。 */
    afe_config->ns_init = false;     /* 关闭降噪，避免降低唤醒和命令识别率。 */
    afe_config->vad_init = true;     /* 检测当前音频块中是否存在语音。 */
    afe_config->agc_init = true;     /* 自动调整近端语音的输出幅度。 */
    afe_config->wakenet_init = true; /* 在 AFE 内运行“你好小鑫”唤醒模型。 */
    afe_config->wakenet_model_name = wakenet_model;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    /*
     * handle 是算法接口函数表，data 是该配置对应的运行实例。配置对象只在
     * create_from_config() 期间使用，实例创建后立即释放，减少初始化占用。
     */
    s_afe_handle = esp_afe_handle_from_config(afe_config);
    if (s_afe_handle == NULL) {
        result = ESP_ERR_NOT_SUPPORTED;
        goto failed;
    }
    s_afe_data = s_afe_handle->create_from_config(afe_config);
    afe_config_free(afe_config);
    afe_config = NULL;
    if (s_afe_data == NULL) {
        result = ESP_ERR_NO_MEM;
        goto failed;
    }
    log_heap_state("After AFE");

    /*
     * 不依赖模型版本的隐含默认值，运行时查询真实格式并严格校验：
     * 采样率必须为 16 kHz、输入必须是 M/R 两通道、feed 块必须有效。
     */
    const int afe_sample_rate = s_afe_handle->get_samp_rate(s_afe_data);
    const int afe_channel_count =
        s_afe_handle->get_feed_channel_num(s_afe_data);
    const int afe_feed_samples =
        s_afe_handle->get_feed_chunksize(s_afe_data);
    if (afe_sample_rate != SPEECH_AFE_SAMPLE_RATE_HZ ||
        afe_channel_count != SPEECH_INPUT_CHANNEL_COUNT ||
        afe_feed_samples <= 0) {
        ESP_LOGE(TAG,
                 "Unexpected AFE format: %d Hz, %d channels, %d samples",
                 afe_sample_rate,
                 afe_channel_count,
                 afe_feed_samples);
        result = ESP_ERR_INVALID_STATE;
        goto failed;
    }
    s_afe_feed_samples = (size_t)afe_feed_samples;

    /* 创建中文命令词识别器；超时时间从唤醒后开始由 MultiNet 统计。 */
    s_multinet = esp_mn_handle_from_name(multinet_model);
    if (s_multinet == NULL) {
        result = ESP_ERR_NOT_SUPPORTED;
        goto failed;
    }
    s_multinet_data = s_multinet->create(multinet_model,
                                         SPEECH_COMMAND_TIMEOUT_MS);
    if (s_multinet_data == NULL) {
        result = ESP_ERR_NO_MEM;
        goto failed;
    }
    log_heap_state("After MultiNet");

    /* AFE fetch 的单声道块必须能原样交给 MultiNet，不允许在中间拼接或截断。 */
    if (s_multinet->get_samp_rate(s_multinet_data) !=
            SPEECH_AFE_SAMPLE_RATE_HZ ||
        s_multinet->get_samp_chunksize(s_multinet_data) !=
            s_afe_handle->get_fetch_chunksize(s_afe_data)) {
        ESP_LOGE(TAG, "AFE and MultiNet frame formats do not match");
        result = ESP_ERR_INVALID_STATE;
        goto failed;
    }
    result = configure_speech_commands();
    if (result != ESP_OK) {
        goto failed;
    }

    /*
     * 大缓冲放 PSRAM：
     *   s_capture_stream_storage = 128 KiB，回调和 Feed 之间的蓄水缓冲
     *   s_callback_interleaved   = 8 KiB，把 MIC1/MIC3 平面数据交错
     *
     * AFE 输入块放内部 RAM：
     *   大小 = feed 样本数 x 2 通道 x 2 字节，由当前模型运行时决定
     */
    s_capture_stream_storage = heap_caps_malloc(
        SPEECH_STREAM_BUFFER_SIZE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_callback_interleaved = heap_caps_malloc(
        SPEECH_CALLBACK_BUFFER_SIZE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    /* AFE feed 输入放在内部 RAM，避免模型处理时访问外部内存。 */
    s_afe_input = heap_caps_malloc(
        s_afe_feed_samples * SPEECH_INPUT_CHANNEL_COUNT * sizeof(int16_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_capture_stream_storage == NULL ||
        s_callback_interleaved == NULL ||
        s_afe_input == NULL) {
        result = ESP_ERR_NO_MEM;
        goto failed;
    }

    /*
     * 使用预分配的 PSRAM 数据区创建静态 StreamBuffer。触发级别 1 表示有任意
     * 字节就可以唤醒接收任务；Feed 任务自行累计到完整 AFE 块后才调用 feed()。
     */
    s_capture_stream = xStreamBufferCreateStatic(
        SPEECH_STREAM_BUFFER_SIZE,
        1,
        s_capture_stream_storage,
        &s_capture_stream_control);
    if (s_capture_stream == NULL) {
        result = ESP_ERR_NO_MEM;
        goto failed;
    }

    /*
     * 长度 1 配合 xQueueOverwrite，只保存尚未执行的最新红灯目标状态。例如
     * Action 任务还没执行“开”时又识别到“关”，队列最终保留“关”。
     */
    s_action_queue = xQueueCreate(1, sizeof(speech_action_t));
    if (s_action_queue == NULL) {
        result = ESP_ERR_NO_MEM;
        goto failed;
    }

    /*
     * Feed 固定 CPU0：从 StreamBuffer 取数据并写入 AFE。
     * Detect 固定 CPU1：从 AFE fetch 并运行 WakeNet/MultiNet 状态机。
     * Action 不绑核：低频执行 AW9523B I2C 操作。
     *
     * || 具有短路特性：任何一个创建失败都会停止创建后续任务；cleanup 会根据
     * 非空 TaskHandle 删除此前已经成功创建的任务。
     */
    if (xTaskCreatePinnedToCore(speech_feed_task,
                                "speech_feed",
                                SPEECH_FEED_TASK_STACK_SIZE,
                                NULL,
                                SPEECH_FEED_TASK_PRIORITY,
                                &s_feed_task,
                                0) != pdPASS ||
        xTaskCreatePinnedToCore(speech_detect_task,
                                "speech_detect",
                                SPEECH_DETECT_TASK_STACK_SIZE,
                                NULL,
                                SPEECH_DETECT_TASK_PRIORITY,
                                &s_detect_task,
                                1) != pdPASS ||
        xTaskCreate(speech_action_task,
                    "speech_action",
                    SPEECH_ACTION_TASK_STACK_SIZE,
                    NULL,
                    SPEECH_ACTION_TASK_PRIORITY,
                    &s_action_task) != pdPASS) {
        result = ESP_ERR_NO_MEM;
        goto failed;
    }

    /*
     * 所有消费者和缓冲都就绪后才接入 audio_recorder。注册成功后，I2S RX
     * 每取得一块数据就会把 MIC1/MIC3 送到 capture_callback()。
     */
    result = audio_recorder_set_aec_input_callback(capture_callback, NULL);
    if (result != ESP_OK) {
        goto failed;
    }
    s_callback_registered = true;
    s_initialized = true;

    ESP_LOGI(TAG,
             "Ready: direct 16 kHz MIC1+MIC3 -> MR, WakeNet=%s, "
             "MultiNet=%s",
             wakenet_model,
             multinet_model);
    log_heap_state("ESP-SR ready");
    s_afe_handle->print_pipeline(s_afe_data);
    return ESP_OK;

failed:
    /* afe_config 尚未交给 AFE 实例时，需要在统一清理前单独释放。 */
    if (afe_config != NULL) {
        afe_config_free(afe_config);
    }
    cleanup_initialization();
    return result;
}
