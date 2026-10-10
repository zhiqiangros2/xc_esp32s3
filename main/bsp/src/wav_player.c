/**
 * @file wav_player.c
 * @brief 后台 PCM WAV 文件播放器。
 *
 * UI 任务只调用 wav_player_play()/wav_player_stop() 投递命令，不直接访问 SD
 * 卡或阻塞写 I2S。SD 播放器任务负责解析 RIFF 和交替填充两个 PSRAM 缓冲；
 * 独立的 DMA 写任务负责把就绪缓冲分成小块持续提交给 I2S。每个后台任务
 * 各自阻塞等待一个静态消息队列；缓冲索引随消息移交，不需要 Queue Set 或
 * 缓冲锁。播放器状态通过临界区保护的快照提供给 LVGL 定时器读取。
 *
 * 两个任务、两个队列的消息方向：
 *
 *   LVGL/UI ---------------- PLAY(path)、STOP -------------------+
 *                                                              v
 *                                                    s_reader_queue
 *                                                              |
 *                                                   wav_reader_task
 *                                               （打开文件、读取 SD）
 *                                                              |
 *                       START(rate)、BUFFER_READY(index)、STOP  |
 *                                                              v
 *                                                    s_writer_queue
 *                                                              |
 *                                                  audio_writer_task
 *                                               （分段写入 I2S DMA）
 *                                                              |
 *        BUFFER_RELEASED(index)、WRITER_STOPPED/FINISHED/ERROR  |
 *                                                              +----> s_reader_queue
 *
 * 双缓冲所有权规则：
 * 1. 读任务填充某个缓冲并发送 BUFFER_READY 后，不得再修改该缓冲；
 * 2. 写任务收到 BUFFER_READY 后分多次写入 I2S；
 * 3. 写任务发送 BUFFER_RELEASED(index) 后，该缓冲才重新归读任务所有；
 * 4. 切歌或停止时，读任务必须等到 WRITER_STOPPED，才能重新使用两个缓冲。
 *
 * 正常播放时序：
 *   先读 buffer1、buffer2 -> START -> 播放 buffer1 -> 释放 buffer1
 *   -> SD 重填 buffer1，同时播放 buffer2 -> 释放 buffer2 -> 继续交替。
 * 没有消息时两个任务都阻塞，不进行固定周期轮询，也不使用 Queue Set。
 *
 * 当前支持格式固定为 RIFF/WAVE、PCM、16 kHz、16 bit、双声道；
 * byte_rate 必须为 16000 x 4 = 64000，block_align 必须为 4。固定采样率
 * 保证播放不会改变录音和语音识别正在使用的公共 I2S 时钟。
 */
#include "wav_player.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "audio_i2s.h"
#include "es8311.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

/* WAV 文件完整路径的最大长度，包含结尾的 '\0'；命令中直接保存路径副本。 */
#define WAV_PLAYER_PATH_MAX 384
/*
 * 两个 PSRAM 缓冲区在 wav_player_init() 中分别独立分配，每个 256 KiB，
 * 总容量为 512 KiB。它们在播放器整个运行期内重复使用，切歌时不重新申请。
 * 以固定的 16 kHz / 16 bit / 双声道计算：
 *   单缓冲容量   = 256 KiB = 256 x 1024 = 262144 字节；
 *   每帧字节数   = 16 bit / 8 x 2 声道 = 4 字节；
 *   每秒数据量   = 16000 帧 x 4 字节 = 64000 字节；
 *   单缓冲时长   = 262144 / 64000 = 4.096 秒；
 *   双缓冲总时长 = 2 x 4.096 = 8.192 秒。
 * DMA 任务播放 buffer1 的约 4.10 秒期间，SD 任务填充 buffer2；随后交换，
 * DMA 任务播放 buffer2 的同时，SD 任务重新填充 buffer1。
 */
#define WAV_PLAYER_BUFFER1_SIZE (256U * 1024U)
#define WAV_PLAYER_BUFFER2_SIZE (256U * 1024U)
#define WAV_PLAYER_BUFFER_COUNT 2U
/*
 * 每次提交给 I2S 驱动的数据量。16 kHz 下：
 *   单次写入时长 = 4096 / 64000 = 0.064 秒，即 64 ms；
 *   每个 256 KiB 缓冲需要 262144 / 4096 = 64 次 I2S 写入。
 * DMA 写任务每次写入前检查一次 STOP 控制，因此停止不必等待整个缓冲约
 * 4.10 秒。SD 任务收到 UI 命令后把 STOP 转发给 DMA 写任务；若当时正在
 * read()，还需等待本次 SD 读取返回。已经进入 DMA 的音频最多约 383 ms。
 */
#define WAV_PLAYER_I2S_WRITE_SIZE 4096U
/* SD 读取/控制任务的静态栈大小（ESP-IDF 的栈单位在本目标上为字节）。 */
#define WAV_PLAYER_READER_TASK_STACK_DEPTH 8192U
/* SD 任务与 LVGL 同级；耗时 read() 不应抢占更实时的 DMA 写任务。 */
#define WAV_PLAYER_READER_TASK_PRIORITY 4
/* DMA 写任务只保存小量状态，4 KiB 静态栈足够。 */
#define WAV_PLAYER_WRITER_TASK_STACK_DEPTH 4096U
/* DMA 写任务优先于 SD 和 LVGL，确保 I2S 持续获得 PCM 数据。 */
#define WAV_PLAYER_WRITER_TASK_PRIORITY 5
/*
 * 单次 I2S 写入等待 DMA 空间的最长时间。正常写入约需 64 ms，100 ms 用于
 * 覆盖短时调度抖动；超时返回时仍按 bytes_written 处理已经写入的部分。
 */
#define WAV_PLAYER_I2S_TIMEOUT_MS 100
/*
 * 读任务队列同时接收 UI 的 PLAY/STOP 和写任务返回的缓冲释放、结束、错误
 * 事件。8 个槽可容纳 4 个连续 UI 操作以及双缓冲回执，不需要轮询多个队列。
 */
#define WAV_PLAYER_READER_QUEUE_LENGTH 8U
/*
 * 写任务队列最多同时保存 START、buffer1、buffer2 和一个高优先级 STOP。
 * STOP 使用 xQueueSendToFront()，可以越过尚未播放的 BUFFER_READY 消息。
 */
#define WAV_PLAYER_WRITER_QUEUE_LENGTH 4U

/*
 * SD 任务填充完成后交给 DMA 写任务的一块 PCM 数据描述。
 * 这里只传递索引和长度，不复制 256 KiB PCM 数据；真正的数据始终保存在
 * s_buffer1/s_buffer2 指向的 PSRAM 中。
 */
typedef struct {
    /* 0 表示 buffer1，1 表示 buffer2。 */
    uint8_t index;
    /* 当前缓冲内有效的 PCM 字节数，文件末块可能不足 256 KiB。 */
    size_t data_size;
    /* true 表示这是 WAV data 区域的最后一块。 */
    bool is_last;
} wav_buffer_block_t;

typedef enum {
    /* UI 请求播放 path；播放中收到时先停止旧文件再切换。 */
    READER_MESSAGE_PLAY,
    /* UI 请求停止当前文件。 */
    READER_MESSAGE_STOP,
    /* 写任务已完整提交一个缓冲，读任务可以按 index 重新填充。 */
    READER_MESSAGE_BUFFER_RELEASED,
    /* STOP 已生效，写任务已不再访问 buffer1/buffer2。 */
    READER_MESSAGE_WRITER_STOPPED,
    /* WAV 最后一块已经完整提交给 I2S DMA。 */
    READER_MESSAGE_WRITER_FINISHED,
    /* I2S、Codec 或消息参数发生错误。 */
    READER_MESSAGE_WRITER_ERROR,
} reader_message_type_t;

/*
 * 读任务唯一的输入消息。不同类型只使用联合体中的一个字段，因此 UI 路径
 * 和高频缓冲回执不会同时占用两份存储。
 */
typedef struct {
    reader_message_type_t type;
    union {
        char path[WAV_PLAYER_PATH_MAX];
        uint8_t buffer_index;
        esp_err_t result;
    } data;
} reader_message_t;

typedef enum {
    /* 根据 sample_rate_hz 配置音频输出并进入工作状态。 */
    WRITER_MESSAGE_START,
    /* 立即停止当前写入，清除本轮尚未处理的缓冲消息。 */
    WRITER_MESSAGE_STOP,
    /* block 描述的 PSRAM 缓冲已经填充完毕，可以写入 I2S。 */
    WRITER_MESSAGE_BUFFER_READY,
} writer_message_type_t;

/*
 * 写任务唯一的输入消息：START 使用 sample_rate_hz，BUFFER_READY 使用 block，
 * STOP 不使用联合体字段。用联合体可让三个事件共用同一段队列存储空间。
 */
typedef struct {
    writer_message_type_t type;
    union {
        uint32_t sample_rate_hz;
        wav_buffer_block_t block;
    } data;
} writer_message_t;

/* 从 WAV 的 fmt 和 data 块提取出的播放参数。 */
typedef struct {
    /* WAV 编码格式，当前只接受 1（线性 PCM）。 */
    uint16_t audio_format;
    /* 当前只接受 2（左右双声道）。 */
    uint16_t channels;
    /* 每声道每秒采样数，同时用于配置 I2S WS 时钟。 */
    uint32_t sample_rate_hz;
    /* 文件声明的每秒 PCM 字节数，用于校验头部一致性。 */
    uint32_t byte_rate;
    /* 一帧左右声道数据占用的字节数，16 bit 双声道应为 4。 */
    uint16_t block_alignment;
    /* 单声道单采样位宽，当前只接受 16。 */
    uint16_t bits_per_sample;
    /* data 块内纯 PCM 数据的总字节数，不包含 WAV 头。 */
    uint32_t data_size;
} wav_format_t;

static const char *TAG = "WAV_PLAYER";
/*
 * SD/控制任务使用静态 TCB 和静态栈，不动态分配任务对象。TaskHandle 用于
 * 判断初始化是否完整，不用于传递播放事件。
 */
static TaskHandle_t s_reader_task;
static StaticTask_t s_reader_task_control_block;
static StackType_t s_reader_task_stack[WAV_PLAYER_READER_TASK_STACK_DEPTH];
/* I2S DMA 写任务同样使用静态 TCB 和静态栈，运行优先级高于读任务。 */
static TaskHandle_t s_writer_task;
static StaticTask_t s_writer_task_control_block;
static StackType_t s_writer_task_stack[WAV_PLAYER_WRITER_TASK_STACK_DEPTH];
/*
 * 两个音频缓冲在 wav_player_init() 中一次性分配。BUFFER_READY 和
 * BUFFER_RELEASED 消息负责转移所有权：同一时刻每块缓冲只属于 SD 任务
 * 或 DMA 写任务，因此无需缓冲锁。
 */
static uint8_t *s_buffer1;
static uint8_t *s_buffer2;
/*
 * 读任务收件队列：UI 和写任务都是生产者，wav_reader_task 是唯一消费者。
 * 任务没有事件时永久阻塞在该队列，不使用超时轮询。
 * storage 数组保存队列项目本体，StaticQueue_t 保存 FreeRTOS 队列控制信息，
 * 因此 xQueueCreateStatic() 不会再从系统堆申请队列内存。
 */
static QueueHandle_t s_reader_queue;
static StaticQueue_t s_reader_queue_control_block;
static uint8_t s_reader_queue_storage[WAV_PLAYER_READER_QUEUE_LENGTH *
                                      sizeof(reader_message_t)];
/*
 * 写任务收件队列：读任务是唯一生产者，audio_writer_task 是唯一消费者。
 * START/BUFFER_READY 从队尾进入，STOP 从队首进入。
 * 队列 FIFO 顺序保证 START 一定先于本轮的 buffer1、buffer2 被处理。
 */
static QueueHandle_t s_writer_queue;
static StaticQueue_t s_writer_queue_control_block;
static uint8_t s_writer_queue_storage[WAV_PLAYER_WRITER_QUEUE_LENGTH *
                                      sizeof(writer_message_t)];
/* 状态快照可能由 SD/控制任务写、LVGL 任务读，必须作为整体同步复制。 */
static wav_player_status_t s_status;
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
/* 标记两个后台任务及其队列已经创建，供公开接口检查调用顺序。 */
static bool s_initialized;
/* 缓存本模块是否已经请求过 Codec 初始化；es8311_init() 本身仍然幂等。 */
static bool s_codec_ready;

/** 从字节流读取一个 RIFF 小端序 16 bit 无符号整数。 */
static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

/** 从字节流读取一个 RIFF 小端序 32 bit 无符号整数。 */
static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

/**
 * @brief 从文件描述符连续读取指定字节数。
 *
 * POSIX read() 允许成功返回少于请求长度的数据，因此 WAV 头和 PCM 缓冲都要
 * 循环读取，直到目标区域填满、遇到 EOF 或真正的 I/O 错误。EINTR 只表示调用
 * 被信号打断，可直接重试。该接口不使用 Newlib FILE，也不会在播放时为 stdio
 * 流动态创建递归锁。
 */
static esp_err_t read_file_exact(int file_descriptor,
                                 void *buffer,
                                 size_t size)
{
    uint8_t *destination = buffer;
    size_t total_read = 0;

    while (total_read < size) {
        const ssize_t read_result = read(file_descriptor,
                                         destination + total_read,
                                         size - total_read);
        if (read_result > 0) {
            total_read += (size_t)read_result;
            continue;
        }
        if (read_result == 0) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (errno != EINTR) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

/**
 * @brief 发布新的播放器状态快照。
 *
 * 对外状态只保存文件名，不保存完整路径，避免 UI 状态结构过大。先在栈上构造
 * 完整快照，再在临界区中一次赋值，缩短关中断时间并防止读到半更新内容。
 */
static void set_status(wav_player_state_t state,
                       esp_err_t error,
                       const char *path)
{
    wav_player_status_t status = {
        .state = state,
        .last_error = error,
    };

    if (path != NULL) {
        /* 路径使用 '/'，取最后一个分隔符后的部分显示在音乐页面。 */
        const char *name = strrchr(path, '/');
        name = name == NULL ? path : name + 1;
        snprintf(status.file_name,
                 sizeof(status.file_name),
                 "%s",
                 name);
    }

    taskENTER_CRITICAL(&s_status_lock);
    s_status = status;
    taskEXIT_CRITICAL(&s_status_lock);
}

/**
 * @brief 解析 WAV 头并把文件位置移动到 data 块起始处。
 *
 * 按 RIFF 数据块顺序查找 fmt 和 data。WAV 头不一定是固定 44 字节，因此
 * 会跳过 LIST、JUNK 等扩展块，并处理 RIFF 对奇数字节块增加的对齐字节。
 * fmt 与 data 不要求固定先后位置；两个块都找到后回到记录的 data 偏移。
 *
 * @param file_descriptor 已以 O_RDONLY 打开的 POSIX 文件描述符。
 * @param format 返回解析并校验后的 PCM 格式。
 * @return ESP_OK 表示支持播放；否则表示文件截断、结构错误或格式不支持。
 */
static esp_err_t parse_wav(int file_descriptor, wav_format_t *format)
{
    /* RIFF 固定头为："RIFF"、文件长度、"WAVE"，共 12 字节。 */
    uint8_t riff_header[12];
    esp_err_t result = read_file_exact(file_descriptor,
                                       riff_header,
                                       sizeof(riff_header));
    if (result != ESP_OK) {
        return result;
    }
    if (memcmp(riff_header, "RIFF", 4) != 0 ||
        memcmp(riff_header + 8, "WAVE", 4) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    bool format_found = false;
    off_t data_offset = (off_t)-1;
    memset(format, 0, sizeof(*format));

    /* 每个子块以 4 字节 ID 和 4 字节小端长度开始。 */
    while (true) {
        uint8_t chunk_header[8];
        result = read_file_exact(file_descriptor,
                                 chunk_header,
                                 sizeof(chunk_header));
        if (result != ESP_OK) {
            return result;
        }

        const uint32_t chunk_size = read_le32(chunk_header + 4);
        const off_t chunk_data_offset = lseek(file_descriptor, 0, SEEK_CUR);
        if (chunk_data_offset == (off_t)-1) {
            return ESP_FAIL;
        }

        if (memcmp(chunk_header, "fmt ", 4) == 0) {
            /* PCM 基础 fmt 内容为 16 字节；更长的扩展内容由后续 lseek 跳过。 */
            uint8_t format_data[16];
            if (chunk_size < sizeof(format_data)) {
                return ESP_ERR_INVALID_SIZE;
            }
            result = read_file_exact(file_descriptor,
                                     format_data,
                                     sizeof(format_data));
            if (result != ESP_OK) {
                return result;
            }

            format->audio_format = read_le16(format_data);
            format->channels = read_le16(format_data + 2);
            format->sample_rate_hz = read_le32(format_data + 4);
            format->byte_rate = read_le32(format_data + 8);
            format->block_alignment = read_le16(format_data + 12);
            format->bits_per_sample = read_le16(format_data + 14);
            format_found = true;
        } else if (memcmp(chunk_header, "data", 4) == 0) {
            /* 此时文件位置已经指向第一个 PCM 字节，只记录位置和长度。 */
            data_offset = chunk_data_offset;
            format->data_size = chunk_size;
        }

        if (format_found && data_offset != (off_t)-1) {
            if (lseek(file_descriptor, data_offset, SEEK_SET) == (off_t)-1) {
                return ESP_FAIL;
            }
            break;
        }

        /* RIFF 子块从偶数字节边界开始，奇数长度块后带一个填充字节。 */
        const uint32_t aligned_size = chunk_size + (chunk_size & 1U);
        if (lseek(file_descriptor,
                  chunk_data_offset + (off_t)aligned_size,
                  SEEK_SET) == (off_t)-1) {
            return ESP_FAIL;
        }
    }

    /*
     * 整条音频链固定为 16 kHz、16 bit 双声道。尤其要在这里拒绝其他采样率，
     * 避免播放器尝试改变常驻 16 kHz RX、ES7210 和 ESP-SR 共用的时钟。
     */
    if (format->audio_format != 1 ||
        format->channels != 2 ||
        format->bits_per_sample != 16 ||
        format->sample_rate_hz != AUDIO_I2S_SAMPLE_RATE_HZ ||
        format->block_alignment != 4 ||
        format->byte_rate != format->sample_rate_hz * 4U) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

/**
 * @brief 确保 I2S 时钟和 ES8311 已准备好接收当前采样率的 PCM。
 *
 * I2S 通道已由 main.c 以固定 16 kHz 创建；这里验证文件采样率并登记播放，
 * 不改变公共时钟。随后确认 ES8311 已初始化；功放使能由 main.c 管理。
 */
static esp_err_t start_audio_output(uint32_t sample_rate_hz)
{
    esp_err_t result = audio_i2s_playback_start(sample_rate_hz);
    if (result != ESP_OK) {
        ESP_LOGE(TAG,
                 "Audio startup stage failed: I2S sample-rate setup: %s",
                 esp_err_to_name(result));
        return result;
    }

    if (!s_codec_ready) {
        result = es8311_init();
        if (result != ESP_OK) {
            ESP_LOGE(TAG,
                     "Audio startup stage failed: ES8311 initialization: %s",
                     esp_err_to_name(result));
            return result;
        }
        s_codec_ready = true;
    }
    return ESP_OK;
}



/** 根据索引取得初始化阶段分配的 PSRAM 音频缓冲。 */
static uint8_t *get_audio_buffer(uint8_t index)
{
    return index == 0 ? s_buffer1 : s_buffer2;
}

/** 根据索引取得对应音频缓冲的容量。 */
static size_t get_audio_buffer_capacity(uint8_t index)
{
    return index == 0 ? WAV_PLAYER_BUFFER1_SIZE : WAV_PLAYER_BUFFER2_SIZE;
}

/**
 * @brief 写任务把运行结果投递到读任务唯一的队列。
 *
 * BUFFER_RELEASED 使用 buffer_index；ERROR 使用 result；STOPPED 和 FINISHED
 * 不需要附加参数。portMAX_DELAY 保证写任务不会悄悄丢失缓冲释放事件，否则
 * 读任务可能永远不知道某个缓冲已经可以覆盖。
 */
static void send_reader_message(reader_message_type_t type,
                                uint8_t buffer_index,
                                esp_err_t result)
{
    reader_message_t message = {.type = type};
    if (type == READER_MESSAGE_BUFFER_RELEASED) {
        message.data.buffer_index = buffer_index;
    } else {
        message.data.result = result;
    }
    (void)xQueueSendToBack(s_reader_queue, &message, portMAX_DELAY);
}

/**
 * @brief STOP 生效后清除写任务队列中属于旧文件的消息。
 *
 * STOP 从队首取出时，队列后面可能仍有旧文件的 buffer1/buffer2。写任务已经
 * 不再访问这些缓冲，所以直接丢弃描述符；随后发送 WRITER_STOPPED，把两个
 * 缓冲的所有权一次性归还给读任务。
 */
static void discard_pending_writer_messages(void)
{
    writer_message_t discarded;
    while (xQueueReceive(s_writer_queue, &discarded, 0) == pdTRUE) {
    }
}

/**
 * @brief 持续消费写任务队列，并按 4096 字节分段写入 I2S DMA。
 *
 * 空闲或等待下一缓冲时永久阻塞在 s_writer_queue。播放一个缓冲期间不取走
 * 后续 BUFFER_READY，只用 xQueuePeek() 检查队首；STOP 从队首插入，所以能在
 * 每次 4096 字节写入前被优先取出。完整写完缓冲后才把其索引发回读任务。
 */
static void audio_writer_task(void *argument)
{
    (void)argument;
    /*
     * active=true 表示已经成功配置采样率，可以接受 BUFFER_READY；false 表示
     * 只接受新的 START/STOP，意外到达的旧 BUFFER_READY 会被丢弃。
     */
    bool active = false;

    while (true) {
        writer_message_t message;
        /*
         * portMAX_DELAY 表示没有消息时永久休眠，不消耗 CPU。START、STOP 或
         * BUFFER_READY 入队时，FreeRTOS 会立即唤醒本任务。
         */
        if (xQueueReceive(s_writer_queue,
                          &message,
                          portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (message.type == WRITER_MESSAGE_STOP) {
            /*
             * 此处可能是空闲状态收到 STOP，也可能是两个缓冲之间收到 STOP。
             * 先清除旧消息，再确认 STOPPED，避免读任务过早覆盖仍在使用的数据。
             */
            active = false;
            (void)audio_i2s_playback_stop();
            discard_pending_writer_messages();
            send_reader_message(READER_MESSAGE_WRITER_STOPPED, 0, ESP_OK);
            continue;
        }

        if (message.type == WRITER_MESSAGE_START) {
            /*
             * START 只负责配置 I2S/Codec，不直接携带 PCM。后续 BUFFER_READY
             * 与 START 使用同一 FIFO 队列，因此不会在采样率配置完成前写数据。
             */
            if (active) {
                active = false;
                (void)audio_i2s_playback_stop();
                send_reader_message(READER_MESSAGE_WRITER_ERROR,
                                    0,
                                    ESP_ERR_INVALID_STATE);
                continue;
            }

            const esp_err_t result =
                start_audio_output(message.data.sample_rate_hz);
            active = result == ESP_OK;
            if (result != ESP_OK) {
                /* start_audio_output() 后半段失败时回滚已登记的播放占用。 */
                (void)audio_i2s_playback_stop();
                send_reader_message(READER_MESSAGE_WRITER_ERROR, 0, result);
            }
            continue;
        }

        /* START 失败后可能已有 READY 消息入队；未激活时直接丢弃旧缓冲。 */
        if (!active || message.type != WRITER_MESSAGE_BUFFER_READY) {
            continue;
        }

        const wav_buffer_block_t block = message.data.block;
        /*
         * 防止错误索引或长度造成 PSRAM 越界。最后一块允许小于 256 KiB，
         * 普通块通常等于对应缓冲容量。
         */
        if (block.index >= WAV_PLAYER_BUFFER_COUNT ||
            block.data_size == 0 ||
            block.data_size > get_audio_buffer_capacity(block.index)) {
            active = false;
            (void)audio_i2s_playback_stop();
            send_reader_message(READER_MESSAGE_WRITER_ERROR,
                                0,
                                ESP_ERR_INVALID_SIZE);
            continue;
        }

        /* offset 表示当前缓冲已经成功提交给 I2S 的字节数。 */
        size_t offset = 0;
        bool stopped = false;
        esp_err_t result = ESP_OK;

        while (offset < block.data_size) {
            /*
             * BUFFER_READY 留在队列里等待当前缓冲完成；STOP 由读任务插入队首，
             * 因此这里只在确实看到 STOP 时才取走消息，不会提前占有下一缓冲。
             */
            writer_message_t queued_message;
            if (xQueuePeek(s_writer_queue,
                           &queued_message,
                           0) == pdTRUE &&
                queued_message.type == WRITER_MESSAGE_STOP) {
                (void)xQueueReceive(s_writer_queue,
                                    &queued_message,
                                    0);
                active = false;
                (void)audio_i2s_playback_stop();
                stopped = true;
                discard_pending_writer_messages();
                send_reader_message(READER_MESSAGE_WRITER_STOPPED,
                                    0,
                                    ESP_OK);
                break;
            }

            const size_t remaining = block.data_size - offset;
            /* 最后一小段可能不足 4096 字节，只提交实际剩余长度。 */
            const size_t write_size =
                remaining < WAV_PLAYER_I2S_WRITE_SIZE
                    ? remaining
                    : WAV_PLAYER_I2S_WRITE_SIZE;
            size_t bytes_written = 0;
            result = audio_i2s_write(get_audio_buffer(block.index) + offset,
                                     write_size,
                                     &bytes_written,
                                     WAV_PLAYER_I2S_TIMEOUT_MS);
            if (bytes_written > 0) {
                offset += bytes_written;
            }

            /* 超时但已经写入部分数据时，下一轮继续提交剩余部分。 */
            if (result == ESP_ERR_TIMEOUT && bytes_written > 0) {
                result = ESP_OK;
                continue;
            }
            if (result != ESP_OK || bytes_written == 0) {
                if (result == ESP_OK) {
                    result = ESP_FAIL;
                }
                ESP_LOGE(TAG,
                         "I2S write failed: result=%s, requested=%u, "
                         "written=%u, buffer%u offset=%u/%u",
                         esp_err_to_name(result),
                         (unsigned)write_size,
                         (unsigned)bytes_written,
                         (unsigned)block.index + 1U,
                         (unsigned)offset,
                         (unsigned)block.data_size);
                break;
            }
        }

        if (stopped) {
            continue;
        }
        if (result != ESP_OK || offset != block.data_size) {
            active = false;
            (void)audio_i2s_playback_stop();
            send_reader_message(READER_MESSAGE_WRITER_ERROR,
                                0,
                                result == ESP_OK ? ESP_FAIL : result);
            continue;
        }

        /*
         * 先归还刚写完的索引，使读任务可以尽早执行下一次 read()。若这是末块，
         * 随后的 FINISHED 表示文件数据已全部提交给 DMA，不表示 Codec 被关闭。
         */
        send_reader_message(READER_MESSAGE_BUFFER_RELEASED,
                            block.index,
                            ESP_OK);
        if (block.is_last) {
            active = false;
            (void)audio_i2s_playback_stop();
            send_reader_message(READER_MESSAGE_WRITER_FINISHED, 0, ESP_OK);
        }
    }
}

/**
 * @brief 把 STOP 插到写任务队首。
 *
 * 若 buffer2 已经在队列中，普通 SendToBack 会让写任务先播放完整个 buffer2，
 * 最坏增加约 1.36 秒停止延迟。SendToFront 让 STOP 在下一个 4096 字节边界被
 * 检查到，停止响应主要由一次 I2S 写入和当前 read() 的耗时决定。
 */
static esp_err_t request_writer_stop(void)
{
    const writer_message_t message = {.type = WRITER_MESSAGE_STOP};
    return xQueueSendToFront(s_writer_queue,
                             &message,
                             portMAX_DELAY) == pdTRUE
               ? ESP_OK
               : ESP_FAIL;
}

/**
 * @brief 从 WAV data 区读取下一段 PCM 到指定缓冲。
 *
 * @param file_descriptor 已定位在 WAV data 区当前读取位置的文件描述符。
 * @param remaining_data_size 输入剩余字节数，成功后减去本次读取量。
 * @param buffer_index 0 选择 buffer1，1 选择 buffer2。
 * @param block 返回给写任务使用的索引、有效长度和末块标志。
 *
 * 目标缓冲位于 PSRAM。sd_fatfs.c 已在 SD host 中注册固定的 4 KiB 片内 DMA
 * 缓冲，底层会自动把这次大读取拆块并中转，不会在运行期申请临时 DMA 内存。
 * 调用者发送 BUFFER_READY 后，才真正把该缓冲的所有权交给写任务。
 */
static esp_err_t fill_audio_buffer(int file_descriptor,
                                   uint32_t *remaining_data_size,
                                   uint8_t buffer_index,
                                   wav_buffer_block_t *block)
{
    if (file_descriptor < 0 || remaining_data_size == NULL || block == NULL ||
        buffer_index >= WAV_PLAYER_BUFFER_COUNT ||
        *remaining_data_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t capacity = get_audio_buffer_capacity(buffer_index);
    /* 最后一段不足一个缓冲时，只读取 WAV data 区实际剩余的字节。 */
    const size_t read_size =
        *remaining_data_size < capacity
            ? *remaining_data_size
            : capacity;
    const esp_err_t result = read_file_exact(file_descriptor,
                                             get_audio_buffer(buffer_index),
                                             read_size);
    if (result != ESP_OK) {
        return result;
    }

    *remaining_data_size -= (uint32_t)read_size;
    *block = (wav_buffer_block_t){
        .index = buffer_index,
        .data_size = read_size,
        .is_last = *remaining_data_size == 0,
    };
    ESP_LOGD(TAG,
             "SD filled buffer%u: %u bytes%s",
             (unsigned)buffer_index + 1U,
             (unsigned)read_size,
             block->is_last ? ", last" : "");
    return ESP_OK;
}

/**
 * @brief 把已填充缓冲的所有权移交给写任务。
 *
 * 消息只复制很小的 wav_buffer_block_t；PCM 本体不进入 FreeRTOS 队列。发送
 * 成功后，读任务必须等到同一 index 的 BUFFER_RELEASED 才能再次写该缓冲。
 */
static esp_err_t send_buffer_ready(const wav_buffer_block_t *block)
{
    const writer_message_t message = {
        .type = WRITER_MESSAGE_BUFFER_READY,
        .data.block = *block,
    };
    return xQueueSendToBack(s_writer_queue,
                            &message,
                            portMAX_DELAY) == pdTRUE
               ? ESP_OK
               : ESP_FAIL;
}

/**
 * @brief 打开 WAV，先填好 buffer1、buffer2，再通知写任务开始。
 *
 * 文件小于一个缓冲时只填 buffer1；其余文件在 START 发出前已经同时准备好
 * 两个缓冲。写任务队列中的顺序固定为 START、buffer1、buffer2。
 *
 * @param path 要播放的绝对路径。
 * @param source_descriptor 若预读后仍有数据，返回打开的描述符；否则返回 -1。
 * @param remaining_data_size 返回尚未读入双缓冲的 PCM 字节数。
 * @param writer_started START 成功入队后置 true，供调用者在后续失败时发 STOP。
 */
static esp_err_t prepare_playback(const char *path,
                                  int *source_descriptor,
                                  uint32_t *remaining_data_size,
                                  bool *writer_started)
{
    *source_descriptor = -1;
    *remaining_data_size = 0;
    *writer_started = false;

    const int file_descriptor = open(path, O_RDONLY);
    if (file_descriptor < 0) {
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    wav_format_t format = {0};
    esp_err_t result = parse_wav(file_descriptor, &format);
    if (result == ESP_OK && format.data_size == 0) {
        result = ESP_ERR_INVALID_SIZE;
    }

    /*
     * 先在 SD 任务中连续填好两个 PSRAM 缓冲。此时写任务尚未收到 START，
     * 因此绝对不会边播放边修改同一个缓冲。
     */
    wav_buffer_block_t initial_blocks[WAV_PLAYER_BUFFER_COUNT];
    size_t initial_block_count = 0;
    uint32_t remaining = format.data_size;
    while (result == ESP_OK &&
           initial_block_count < WAV_PLAYER_BUFFER_COUNT &&
           remaining > 0) {
        result = fill_audio_buffer(file_descriptor,
                                   &remaining,
                                   (uint8_t)initial_block_count,
                                   &initial_blocks[initial_block_count]);
        if (result == ESP_OK) {
            ++initial_block_count;
        }
    }

    if (result == ESP_OK) {
        /* 两个初始缓冲准备完成后，才允许写任务启动音频输出。 */
        const writer_message_t start = {
            .type = WRITER_MESSAGE_START,
            .data.sample_rate_hz = format.sample_rate_hz,
        };
        if (xQueueSendToBack(s_writer_queue,
                             &start,
                             portMAX_DELAY) != pdTRUE) {
            result = ESP_FAIL;
        } else {
            *writer_started = true;
        }
    }

    /* 保持 buffer1、buffer2 顺序，第一次播放一定从 buffer1 开始。 */
    for (size_t i = 0; result == ESP_OK && i < initial_block_count; ++i) {
        result = send_buffer_ready(&initial_blocks[i]);
    }

    if (result != ESP_OK) {
        close(file_descriptor);
        return result;
    }

    ESP_LOGI(TAG,
             "Playing %s: %" PRIu32 " Hz, data=%" PRIu32 " bytes",
             path,
             format.sample_rate_hz,
             format.data_size);
    *remaining_data_size = remaining;
    if (remaining > 0) {
        *source_descriptor = file_descriptor;
    } else {
        close(file_descriptor);
    }
    return ESP_OK;
}

typedef enum {
    /* 当前没有等待 STOPPED 后执行的动作。 */
    READER_PENDING_NONE,
    /* STOPPED 后打开 pending_path，并启动新文件；用于首次播放和切歌。 */
    READER_PENDING_PLAY,
    /* STOPPED 后把公开状态改为 WAV_PLAYER_STOPPED。 */
    READER_PENDING_STOP,
    /* STOPPED 后保留已经发布的 WAV_PLAYER_ERROR 状态。 */
    READER_PENDING_ERROR,
} reader_pending_action_t;

/**
 * @brief SD 读取、双缓冲管理和播放状态任务。
 *
 * 本任务始终用 portMAX_DELAY 阻塞等待 s_reader_queue，一个队列同时承载 UI
 * 命令和写任务回执：
 * 1. PLAY：若旧播放仍在运行，先发 STOP；收到 STOPPED 后读取 buffer1、
 *    buffer2，两个缓冲准备好后再依次发送 START、BUFFER_READY。
 * 2. BUFFER_RELEASED(index)：表示该缓冲已经完整写入 DMA；从 SD 读取下一段
 *    数据到同一个 index，再发送 BUFFER_READY，另一个缓冲可继续播放。
 * 3. STOP：关闭文件并向写任务队首发送 STOP；收到 STOPPED 后进入停止状态。
 * 4. FINISHED/ERROR：关闭文件并更新公开状态，不进行周期轮询。
 */
static void wav_reader_task(void *argument)
{
    (void)argument;

    /*
     * file_descriptor 只由本任务访问。预读完全部文件后会提前关闭；写任务仍可
     * 播放已经位于 PSRAM 中的最后一个或两个缓冲。
     */
    int file_descriptor = -1;
    /* WAV data 区中尚未通过 read() 读入 PSRAM 的字节数。 */
    uint32_t remaining_data_size = 0;
    /*
     * writer_active 是读任务对写任务状态的本地记录：START 入队后置 true，
     * 收到 STOPPED、FINISHED 或 ERROR 后置 false。
     */
    bool writer_active = false;
    /*
     * true 表示 STOP 已发出但尚未收到 WRITER_STOPPED。期间不能覆盖任一缓冲，
     * 因为写任务可能还在完成当前 4096 字节 I2S 写入。
     */
    bool waiting_for_stop = false;
    /* 保存取得两个缓冲所有权后需要执行的下一步：播放、停止或保留错误。 */
    reader_pending_action_t pending_action = READER_PENDING_NONE;
    /* 当前已启动文件用于日志和错误状态；pending_path 保存切歌目标。 */
    char current_path[WAV_PLAYER_PATH_MAX] = {0};
    char pending_path[WAV_PLAYER_PATH_MAX] = {0};

    while (true) {
        reader_message_t message;
        if (xQueueReceive(s_reader_queue,
                          &message,
                          portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (message.type) {
            case READER_MESSAGE_PLAY:
                /*
                 * 新 PLAY 会替换尚未执行的旧 PLAY。先停止继续读取旧文件；若
                 * 写任务仍持有缓冲，循环后半段会发 STOP 并等待安全确认。
                 */
                if (file_descriptor >= 0) {
                    close(file_descriptor);
                    file_descriptor = -1;
                }
                remaining_data_size = 0;
                pending_action = READER_PENDING_PLAY;
                snprintf(pending_path,
                         sizeof(pending_path),
                         "%s",
                         message.data.path);
                set_status(WAV_PLAYER_LOADING, ESP_OK, pending_path);
                break;

            case READER_MESSAGE_STOP:
                /*
                 * STOP 不直接在 UI 上下文关闭文件。读任务收到消息后关闭 SD
                 * 文件，并把待执行动作改成 STOP；当前 DMA 写入由写任务结束。
                 */
                if (file_descriptor >= 0) {
                    close(file_descriptor);
                    file_descriptor = -1;
                }
                remaining_data_size = 0;
                pending_action = READER_PENDING_STOP;
                pending_path[0] = '\0';
                break;

            case READER_MESSAGE_BUFFER_RELEASED:
                /*
                 * index 指明刚播放完成的是 buffer1 还是 buffer2。只重填这一块，
                 * 另一块仍归写任务所有并继续输出，两个任务因此可以并行工作。
                 */
                if (file_descriptor >= 0 && remaining_data_size > 0 &&
                    !waiting_for_stop) {
                    wav_buffer_block_t block;
                    esp_err_t result =
                        fill_audio_buffer(file_descriptor,
                                          &remaining_data_size,
                                          message.data.buffer_index,
                                          &block);
                    if (result == ESP_OK) {
                        result = send_buffer_ready(&block);
                    }
                    if (result != ESP_OK) {
                        close(file_descriptor);
                        file_descriptor = -1;
                        remaining_data_size = 0;
                        pending_action = READER_PENDING_ERROR;
                        ESP_LOGE(TAG,
                                 "SD read failed for %s: %s",
                                 current_path,
                                 esp_err_to_name(result));
                        set_status(WAV_PLAYER_ERROR,
                                   result,
                                   current_path);
                    } else if (remaining_data_size == 0) {
                        /* 最后一块已移交；关闭文件并等待 FINISHED。 */
                        close(file_descriptor);
                        file_descriptor = -1;
                    }
                }
                break;

            case READER_MESSAGE_WRITER_STOPPED:
                /*
                 * 到这里写任务已经停止访问两个 PSRAM 缓冲。循环后半段现在可
                 * 安全执行 pending_action，例如预读下一首歌曲。
                 */
                writer_active = false;
                waiting_for_stop = false;
                break;

            case READER_MESSAGE_WRITER_FINISHED:
                /*
                 * 末块已经全部提交给 I2S DMA。正常结束时转为 STOP；若此前已经
                 * 请求切歌/停止，则保留 pending_action，继续等待 STOPPED 回执。
                 */
                writer_active = false;
                if (file_descriptor >= 0) {
                    close(file_descriptor);
                    file_descriptor = -1;
                }
                remaining_data_size = 0;
                if (!waiting_for_stop &&
                    pending_action == READER_PENDING_NONE) {
                    pending_action = READER_PENDING_STOP;
                }
                break;

            case READER_MESSAGE_WRITER_ERROR:
                /*
                 * 写任务报错后不会继续访问当前缓冲。若正在等待主动 STOP，则
                 * 队列中的 STOP 随后仍会产生 STOPPED，并执行原来的待定动作。
                 */
                writer_active = false;
                if (file_descriptor >= 0) {
                    close(file_descriptor);
                    file_descriptor = -1;
                }
                remaining_data_size = 0;
                if (!waiting_for_stop) {
                    pending_action = READER_PENDING_ERROR;
                    ESP_LOGE(TAG,
                             "DMA writer failed for %s: %s",
                             current_path,
                             esp_err_to_name(message.data.result));
                    set_status(WAV_PLAYER_ERROR,
                               message.data.result,
                               current_path);
                }
                break;

            default:
                break;
        }

        /*
         * PLAY、STOP 或 SD 读取错误都要先取得两个缓冲的完整所有权。只发送
         * 一次 STOP；waiting_for_stop 防止重复消息占满写任务队列。
         */
        if (writer_active &&
            pending_action != READER_PENDING_NONE &&
            !waiting_for_stop) {
            const esp_err_t result = request_writer_stop();
            if (result == ESP_OK) {
                waiting_for_stop = true;
            } else {
                pending_action = READER_PENDING_ERROR;
                set_status(WAV_PLAYER_ERROR, result, current_path);
            }
        }

        /* 仍在等待 STOPPED 时不访问 buffer1/buffer2。 */
        if (writer_active || waiting_for_stop) {
            continue;
        }

        if (pending_action == READER_PENDING_PLAY) {
            /*
             * 只有 writer_active=false 且未等待 STOPPED 才会到达这里，因此
             * buffer1、buffer2 都已归读任务所有，可以连续预读而不会数据竞争。
             */
            snprintf(current_path,
                     sizeof(current_path),
                     "%s",
                     pending_path);
            pending_action = READER_PENDING_NONE;

            bool writer_started = false;
            const esp_err_t result =
                prepare_playback(current_path,
                                 &file_descriptor,
                                 &remaining_data_size,
                                 &writer_started);
            writer_active = writer_started;
            if (result == ESP_OK) {
                /* START 和初始缓冲已经入队，实际 I2S 写入由高优先级任务执行。 */
                set_status(WAV_PLAYER_PLAYING, ESP_OK, current_path);
            } else {
                pending_action = writer_started
                                     ? READER_PENDING_ERROR
                                     : READER_PENDING_NONE;
                ESP_LOGE(TAG,
                         "Cannot start playback for %s: %s",
                         current_path,
                         esp_err_to_name(result));
                set_status(WAV_PLAYER_ERROR, result, current_path);
                if (writer_started) {
                    /*
                     * START 已入队但后续步骤失败时必须再发 STOP，等写任务确认
                     * 后才能保证两个缓冲重新归读任务所有。
                     */
                    const esp_err_t stop_result = request_writer_stop();
                    if (stop_result == ESP_OK) {
                        waiting_for_stop = true;
                    }
                }
            }
        } else if (pending_action == READER_PENDING_STOP) {
            /* STOPPED 已确认或文件自然结束，现在对外发布稳定停止状态。 */
            pending_action = READER_PENDING_NONE;
            current_path[0] = '\0';
            set_status(WAV_PLAYER_STOPPED, ESP_OK, NULL);
        } else if (pending_action == READER_PENDING_ERROR) {
            /* 错误状态已在发生位置发布，这里只清除内部待定标记。 */
            pending_action = READER_PENDING_NONE;
        }
    }
}

/**
 * @brief 分配双缓冲、两个静态队列并创建 SD/DMA 两个后台任务。
 *
 * 函数可重复调用。读任务和写任务各有一个静态收件队列；buffer1 和 buffer2
 * 从 PSRAM 分配一次，后续所有 PLAY 共用。SD 所需的片内 DMA 中转区由
 * sd_fatfs.c 在挂载时统一管理。这里只准备播放器资源，不改变 I2S/Codec 状态。
 *
 * 初始化顺序：
 * 1. 创建 reader/writer 两个静态队列；
 * 2. 从 PSRAM 分配两个 256 KiB PCM 缓冲；
 * 3. 先创建高优先级写任务，使其阻塞等待 START；
 * 4. 再创建读任务，使其阻塞等待 UI 的 PLAY/STOP；
 * 5. 所有资源成功后才设置 s_initialized，避免公开接口使用半初始化对象。
 */
esp_err_t wav_player_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    memset(&s_status, 0, sizeof(s_status));
    s_status.state = WAV_PLAYER_STOPPED;

    /*
     * 两个队列的控制块和项目存储都由文件级静态数组提供。这里仅让 FreeRTOS
     * 初始化这些内存，不涉及运行期堆分配，也不需要 Queue Set。
     */
    s_reader_queue =
        xQueueCreateStatic(WAV_PLAYER_READER_QUEUE_LENGTH,
                           sizeof(reader_message_t),
                           s_reader_queue_storage,
                           &s_reader_queue_control_block);
    s_writer_queue =
        xQueueCreateStatic(WAV_PLAYER_WRITER_QUEUE_LENGTH,
                           sizeof(writer_message_t),
                           s_writer_queue_storage,
                           &s_writer_queue_control_block);
    if (s_reader_queue == NULL || s_writer_queue == NULL) {
        s_writer_queue = NULL;
        s_reader_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    /*
     * 提前准备两个独立 PSRAM 缓冲。收到 PLAY 后先连续填充 buffer1、buffer2，
     * 两块都准备好再启动写任务，不在播放路径中承担大块内存分配延迟。
     */
    s_buffer1 = heap_caps_malloc(WAV_PLAYER_BUFFER1_SIZE,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_buffer1 == NULL) {
        s_writer_queue = NULL;
        s_reader_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_buffer2 = heap_caps_malloc(WAV_PLAYER_BUFFER2_SIZE,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_buffer2 == NULL) {
        heap_caps_free(s_buffer1);
        s_buffer1 = NULL;
        s_writer_queue = NULL;
        s_reader_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    /*
     * 先启动更高优先级的 DMA 写任务。刚启动时 s_writer_queue 为空，所以任务
     * 会永久阻塞，不会在尚未收到 PLAY 时访问 I2S 或占用 CPU。
     */
    s_writer_task =
        xTaskCreateStatic(audio_writer_task,
                          "wav_writer",
                          WAV_PLAYER_WRITER_TASK_STACK_DEPTH,
                          NULL,
                          WAV_PLAYER_WRITER_TASK_PRIORITY,
                          s_writer_task_stack,
                          &s_writer_task_control_block);
    if (s_writer_task == NULL) {
        heap_caps_free(s_buffer2);
        heap_caps_free(s_buffer1);
        s_buffer2 = NULL;
        s_buffer1 = NULL;
        s_writer_queue = NULL;
        s_reader_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    /*
     * 再启动 SD/命令任务。它同样先阻塞在 s_reader_queue，直到 UI 投递第一条
     * PLAY/STOP；8 KiB 栈用于文件解析、路径副本和两个初始缓冲描述符。
     */
    s_reader_task = xTaskCreateStatic(wav_reader_task,
                                      "wav_reader",
                                      WAV_PLAYER_READER_TASK_STACK_DEPTH,
                                      NULL,
                                      WAV_PLAYER_READER_TASK_PRIORITY,
                                      s_reader_task_stack,
                                      &s_reader_task_control_block);
    if (s_reader_task == NULL) {
        vTaskDelete(s_writer_task);
        s_writer_task = NULL;
        heap_caps_free(s_buffer2);
        heap_caps_free(s_buffer1);
        s_buffer2 = NULL;
        s_buffer1 = NULL;
        s_writer_queue = NULL;
        s_reader_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,
             "Player ready: SD stack=%u, writer stack=%u, "
             "buffer1=%u, buffer2=%u bytes",
             (unsigned)sizeof(s_reader_task_stack),
             (unsigned)sizeof(s_writer_task_stack),
             (unsigned)WAV_PLAYER_BUFFER1_SIZE,
             (unsigned)WAV_PLAYER_BUFFER2_SIZE);
    s_initialized = true;
    return ESP_OK;
}

/**
 * @brief 异步请求播放一个 WAV 文件。
 *
 * 本函数可由 LVGL 任务调用，不执行文件 I/O。成功仅表示命令已提交，不代表
 * 文件一定存在或格式一定受支持；最终结果需要通过 wav_player_get_status()
 * 查询。事件按 FIFO 入队；SD/控制任务收到新的 PLAY 后会关闭当前文件，停止
 * DMA 写任务并等待缓冲释放确认，然后再读取新文件。
 *
 * path 会完整复制进队列消息，所以函数返回后调用者可以立即释放或复用原始
 * 字符串。发送使用 0 tick，UI 不会因 SD 卡读取或队列等待而阻塞。
 */
esp_err_t wav_player_play(const char *path)
{
    if (!s_initialized || s_reader_task == NULL || s_writer_task == NULL ||
        s_reader_queue == NULL || s_writer_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (path == NULL || path[0] == '\0' || strlen(path) >= WAV_PLAYER_PATH_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    reader_message_t message = {.type = READER_MESSAGE_PLAY};
    snprintf(message.data.path, sizeof(message.data.path), "%s", path);
    /* 0 tick 表示 UI 不等待；队列已满时立即返回超时错误。 */
    if (xQueueSendToBack(s_reader_queue, &message, 0) != pdPASS) {
        return ESP_ERR_TIMEOUT;
    }
    set_status(WAV_PLAYER_LOADING, ESP_OK, path);
    return ESP_OK;
}

/**
 * @brief 异步请求停止当前文件播放。
 *
 * 停止命令不会关闭 I2S、Codec 或板级功放，避免下一次播放重新上电产生爆音。
 * 本函数只把 STOP 放入读任务队列；真正的文件关闭、写任务停止和缓冲回收均
 * 在后台完成。调用返回 ESP_OK 不等于音频已经在该时刻完全静音。
 */
esp_err_t wav_player_stop(void)
{
    if (!s_initialized || s_reader_task == NULL || s_writer_task == NULL ||
        s_reader_queue == NULL || s_writer_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const reader_message_t message = {.type = READER_MESSAGE_STOP};
    return xQueueSendToBack(s_reader_queue, &message, 0) == pdPASS
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

/**
 * @brief 取得播放器状态的一致性快照。
 *
 * 复制过程由短临界区保护，调用者获得的结构随后独立于内部状态，可安全用于
 * LVGL 文本更新。函数不等待播放任务，也不访问 SD 卡。
 * 临界区只复制一个小结构，不允许在其中执行日志、文件 I/O 或队列等待。
 */
esp_err_t wav_player_get_status(wav_player_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    taskENTER_CRITICAL(&s_status_lock);
    *status = s_status;
    taskEXIT_CRITICAL(&s_status_lock);
    return ESP_OK;
}
