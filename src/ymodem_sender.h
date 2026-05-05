/**
 * @file ymodem_sender.h
 * @brief Ymodem 协议发送器头文件
 *
 * 定义 Ymodem 协议发送器的数据结构、枚举类型和公开 API。
 * 协议公共定义（帧结构宏、枚举、CRC 接口等）请参见 ymodem_common.h 。
 *
 * @par 会话结束机制
 * 发送器通过 file_index 自动递增追踪文件序号。
 * 用户在 FILE_INFO 事件回调中判断 event->file_index 与本地文件列表是否匹配：
 * - 有下一个文件：填充 file_name 和 file_total_size
 * - 没有更多文件：将 file_name[0] 设为 '\0'，发送器自动发送空文件名包结束会话
 *
 * @par 典型使用流程
 * @code
 *   ymodem_sender_create(&sender, tx_buf, sizeof(tx_buf));
 *   ymodem_sender_set_event_callback(&sender, my_event_cb, NULL);
 *   ymodem_sender_set_send_packet_callback(&sender, my_send_cb, NULL);
 *   ymodem_sender_start(&sender);
 *   while (!done) {
 *       len = read_response_from_channel(buf, sizeof(buf));
 *       ymodem_sender_parse(&sender, buf, len);
 *       ymodem_sender_poll(&sender);
 *   }
 * @endcode
 */

#pragma once
#include "ymodem_common.h"

typedef struct ymodem_sender ymodem_sender_t;
typedef struct ymodem_sender_event ymodem_sender_event_t;

/**
 * @brief 发送器事件通知回调
 *
 * 发送器在需要用户提供数据或通知状态变更时调用。
 *
 * @param send     发送器实例
 * @param event    事件信息（含 event->type 及对应字段）
 * @param user_ctx 用户注册的上下文指针
 */
typedef void (*ymodem_sender_event_callback_t)(ymodem_sender_t* send,
    ymodem_sender_event_t* event,
    void* user_ctx);

/**
 * @brief 发送数据包回调函数类型
 *
 * 当发送器构造完成一帧数据后，调用此回调通过物理通道发送。
 * 待发送数据位于 tx_buffer，长度为 tx_buffer_active_len。
 *
 * @param send       发送器实例
 * @param send_event 发送事件（包含待发送数据信息）
 * @param user_ctx   用户上下文指针
 */
typedef void (*ymodem_sender_send_packet_t)(ymodem_sender_t* send,
    ymodem_sender_event_t* send_event,
    void* user_ctx);

/** @brief 发送器响应处理状态 */
typedef enum
{
    YMODEM_SENDER_WAIT_ACK = 0,  /**< 等待 ACK（或 C） */
    YMODEM_SENDER_WAIT_C,        /**< 等待字符 'C' */
    YMODEM_SENDER_WAIT_CAN_2,    /**< 等待第二个 CAN 字节 */
} ymodem_sender_stat_e;

/** @brief 发送器文件信息 */
typedef struct
{
    uint32_t file_index;              /**< 当前文件索引（每完成一个文件自动递增） */
    char     file_name[128];          /**< 文件名（'\0' 结尾，空串表示会话结束） */
    uint32_t file_total_size;         /**< 文件总大小（字节），0 表示空文件 */
    uint32_t file_send_size;          /**< 已发送字节数 */
    uint32_t file_send_frame_number;  /**< 已发送帧序号 */
} ymodem_sender_file_info_t;

/** @brief 发送器帧信息 */
typedef struct
{
    ymodem_frame_type_e frame_type;                   /**< 帧类型 */
    uint32_t           current_frame_total_len;       /**< 当前帧总长度（字节） */
    uint32_t           current_frame_data_len;        /**< 当前帧数据区容量 */
    uint32_t           current_frame_data_active_len; /**< 当前帧数据实际有效长度 */
    uint32_t           current_frame_error_count;     /**< 当前帧重传计数 */
} ymodem_sender_frame_info_t;

/** @brief 发送器缓冲区 */
typedef struct
{
    uint8_t* tx_buffer;            /**< 发送缓冲区指针（由用户分配） */
    uint32_t tx_buffer_active_len; /**< 本次待发送数据长度 */
} ymodem_sender_buffer_t;

/** @brief 发送器回调函数集合 */
typedef struct {
    ymodem_sender_event_callback_t send_event_callback;     /**< 事件通知回调 */
    void*                          send_event_user_ctx;     /**< 事件回调用户上下文 */
    ymodem_sender_send_packet_t    send_packet;             /**< 数据包发送回调 */
    void*                          send_packet_user_ctx;    /**< 发送回调用户上下文 */
} ymodem_sender_callbacks_t;

/** @brief 发送器协议处理状态 */
typedef struct {
    uint32_t last_time_ms;        /**< 上次发包时间戳（毫秒），用于超时计算 */
} ymodem_sender_process_t;

/** @brief 发送器用户事件类型 */
typedef enum {
    YMODEM_SENDER_EVENT_FILE_INFO,          /**< 请求用户填充文件名和大小 */
    YMODEM_SENDER_EVENT_DATA_PACKET,        /**< 请求用户填充数据到 tx_buffer */
    YMODEM_SENDER_EVENT_TRANSFER_COMPLETE,  /**< 当前文件传输完成，用户应关闭文件 */
    YMODEM_SENDER_EVENT_SESSION_FINISHED,   /**< 会话结束（已发送空文件名包） */
    YMODEM_SENDER_EVENT_ERROR,              /**< 传输错误/取消 */
} ymodem_sender_event_type_t;

/** @brief 发送器事件详情 */
struct ymodem_sender_event {
    ymodem_sender_event_type_t type;       /**< 事件类型 */
    uint32_t  file_index;                  /**< 文件索引号 */
    char*     file_name;                   /**< 文件名（仅 FILE_INFO 有效） */
    uint32_t  file_size;                   /**< 文件总大小（仅 FILE_INFO 有效） */
    uint8_t*  data;                        /**< 数据指针（仅 DATA_PACKET 有效，指向 tx_buffer 数据区） */
    uint32_t  data_seq;                    /**< 数据包序号（仅 DATA_PACKET 有效） */
    uint32_t  data_len;                    /**< 数据长度（仅 DATA_PACKET 有效，用户应赋值） */
};

/** @brief Ymodem 发送器主结构体 */
struct ymodem_sender
{
    ymodem_sender_file_info_t  file_info;    /**< 文件信息 */
    ymodem_sender_frame_info_t frame_info;   /**< 帧构建信息 */
    ymodem_sender_buffer_t     buffer;       /**< 发送缓冲区 */
    ymodem_sender_callbacks_t  callbacks;    /**< 回调函数集合 */
    ymodem_sender_stat_e       stat;         /**< 响应处理状态 */
    ymodem_error_e             error;        /**< 当前错误码 */
    ymodem_stage_e             stage;        /**< 协议阶段 */
    ymodem_sender_process_t    process;      /**< 协议处理状态 */
    ymodem_sender_event_t      user_evt;     /**< 用户通知事件缓存 */
    bool                       data_1k_enable; /**< 是否启用 1024 字节 STX 帧 */
};

/**
 * @brief 创建 Ymodem 协议发送器
 *
 * @param send           未初始化的发送器实例
 * @param tx_buffer      发送缓冲区（至少 YMODEM_STX_FRAME_LEN_BYTE 字节）
 * @param tx_buffer_size 发送缓冲区大小
 * @return true 创建成功
 * @return false 参数非法
 */
bool ymodem_sender_create(ymodem_sender_t* send, uint8_t* tx_buffer, uint32_t tx_buffer_size);

/**
 * @brief 设置事件通知回调
 *
 * @param send     发送器实例
 * @param callback 事件回调函数
 * @param user_ctx 用户上下文（透传至回调）
 * @return true 成功
 * @return false send 为 NULL
 */
bool ymodem_sender_set_event_callback(ymodem_sender_t* send, ymodem_sender_event_callback_t callback, void* user_ctx);

/**
 * @brief 设置数据包发送回调
 *
 * @param send     发送器实例
 * @param comm_cb  发送回调函数
 * @param user_ctx 用户上下文（透传至回调）
 * @return true 成功
 * @return false send 为 NULL
 */
bool ymodem_sender_set_send_packet_callback(ymodem_sender_t* send,
    ymodem_sender_send_packet_t comm_cb, void* user_ctx);

/**
 * @brief Ymodem 发送器响应解析入口
 *
 * 非阻塞式。处理接收方返回的 ACK/NAK/C/CAN 等单字节响应，驱动协议状态机。
 *
 * @param send 发送器实例
 * @param data 接收方响应数据
 * @param len  数据长度
 * @return ymodem_error_e 本次调用的处理结果：
 *         - ::YMODEM_ERROR_NONE    响应被成功处理
 *         - ::YMODEM_ERROR_GARBAGE   数据非 Ymodem 响应，责任链应解锁
 *         - ::YMODEM_ERROR_WAIT_MORE 未触发处理（如等待第二个 CAN）
 *         - 其他错误码              重发/超限/CAN 取消等
 */
ymodem_error_e ymodem_sender_parse(ymodem_sender_t* send, const uint8_t* data, uint32_t len);

/**
 * @brief Ymodem 发送器超时轮询
 *
 * 需在主循环中定期调用。检测发包后是否在 YMODEM_TIMEOUT_MS 内收到响应。
 * 超时后自动触发重传逻辑。
 *
 * @param send 发送器实例
 * @return true  触发了超时处理（重传或发送 CAN）
 * @return false 未超时、IDLE 状态或 send 为 NULL
 */
bool ymodem_sender_poll(ymodem_sender_t* send);

/**
 * @brief 启动 Ymodem 发送器
 *
 * 进入 ESTABLISHING 阶段等待接收方 'C' 字符。
 *
 * @param send 发送器实例
 * @return true  启动成功
 * @return false send 为 NULL
 */
bool ymodem_sender_start(ymodem_sender_t* send);

/**
 * @brief 启用 1K 数据包模式
 *
 * 调用后发送器将使用 1024 字节 STX 数据帧代替默认的 128 字节 SOH 帧。
 * 需在 ymodem_sender_create 之后、ymodem_sender_start 之前调用。
 *
 * @param send 发送器实例
 */
void ymodem_sender_enable_1k(ymodem_sender_t* send);

/**
 * @brief 复位 Ymodem 发送器内部状态
 *
 * 根据当前错误类型和协议阶段执行分级复位。
 *
 * @param send 发送器实例
 */
void ymodem_sender_reset(ymodem_sender_t* send);
