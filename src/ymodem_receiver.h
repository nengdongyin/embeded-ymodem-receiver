/**
 * @file ymodem_receiver.h
 * @brief Ymodem 协议接收器头文件
 *
 * 定义 Ymodem 协议接收器的数据结构、枚举类型和公开 API。
 * 协议公共定义（帧结构宏、枚举、CRC 接口等）请参见 ymodem_common.h 。
 *
 * @par 典型使用流程
 * @code
 *   ymodem_receiver_create(&parser, rx_buf, sizeof(rx_buf));
 *   ymodem_receiver_set_event_callback(&parser, my_event_cb, NULL);
 *   ymodem_receiver_set_send_response_callback(&parser, my_send_cb, NULL);
 *   ymodem_receiver_start(&parser);
 *   while (!done) {
 *       len = read_from_channel(buf, sizeof(buf));
 *       ymodem_receiver_parse(&parser, buf, len);
 *       ymodem_receiver_poll(&parser);
 *   }
 * @endcode
 */

#pragma once
#include "ymodem_common.h"
typedef struct ymodem_receiver_parser ymodem_receiver_parser_t;
typedef struct ymodem_receiver_event ymodem_receiver_event_t;

/**
 * @brief 接收器事件通知回调函数类型
 *
 * 当接收器完成一个数据包或发生状态变更时通知用户。
 * 用户在回调中读取事件信息并执行相应操作（打开文件、写数据等）。
 *
 * @param parser   解析器实例
 * @param event    事件详细信息
 * @param user_ctx 用户注册的上下文指针
 */
typedef void (*ymodem_receiver_event_callback_t)(ymodem_receiver_parser_t* parser,
    const ymodem_receiver_event_t* event,
    void* user_ctx);

/**
 * @brief 发送响应回调函数类型
 *
 * 当接收器需要向发送端发送应答（ACK/NAK/C/CAN）时调用。
 * 用户通过 tx_buffer + tx_buffer_ack_len 获取待发送数据。
 *
 * @param parser   解析器实例
 * @param user_ctx 用户注册的上下文指针
 */
typedef void (*ymodem_receiver_send_response_t)(ymodem_receiver_parser_t* parser, void* user_ctx);

/** @brief 接收器字节级解析状态 */
typedef enum
{
    YMODEM_RECV_WAIT_HEAD = 0, /**< 等待帧头字节（SOH/STX/EOT/CAN） */
    YMODEM_RECV_WAIT_SEQ,      /**< 等待序号 + 反码字节 */
    YMODEM_RECV_WAIT_DATA,     /**< 等待数据区字节 */
    YMODEM_RECV_WAIT_CRC,      /**< 等待 CRC16 校验码 */
    YMODEM_RECV_WAIT_CAN_2,    /**< 等待第二个 CAN 字节 */
} ymodem_receiver_file_parse_stat_e;

/** @brief 接收器文件信息 */
typedef struct
{
    bool     file_is_active;        /**< 文件是否处于接收状态 */
    char     file_name[128];        /**< 文件名（以 '\0' 结尾） */
    uint32_t file_total_size;       /**< 文件总大小（字节） */
    uint32_t file_rev_size;         /**< 已接收字节数 */
    uint32_t file_rev_frame_number; /**< 期望的下一个帧序号 */
} ymodem_receiver_file_info_t;

/** @brief 接收器当前帧解析信息 */
typedef struct
{
    bool               frame_is_start;            /**< 帧接收已开始（用于 poll 超时检测） */
    bool               frame_is_end;              /**< 帧处理已完成，等待下一帧头触发复位 */
    bool               current_frame_is_resend;   /**< 当前帧是否为重传帧 */
    ymodem_frame_type_e frame_type;               /**< 帧类型标识 */
    uint32_t           current_frame_index;       /**< 当前帧序号 */
    uint32_t           current_frame_total_len;   /**< 当前帧预期总长度（含帧头+数据+CRC） */
    uint32_t           current_frame_data_len;    /**< 当前帧数据区容量 */
    uint32_t           current_frame_rev_len;     /**< 已接收字节数 */
    uint32_t           current_frame_error_count; /**< 当前帧累计错误重传次数 */
} ymodem_receiver_frame_info_t;

/** @brief 接收器缓冲区 */
typedef struct
{
    uint8_t* rx_buffer;        /**< 接收缓冲区指针（由用户分配） */
    uint32_t rx_buffer_len;    /**< 接收缓冲区容量 */
    uint8_t  tx_buffer[4];     /**< 应答发送缓冲区（ACK/NAK/C/CAN） */
    uint32_t tx_buffer_len;    /**< 应答发送缓冲区容量（固定 4） */
    uint32_t tx_buffer_ack_len;/**< 本次待发送的应答数据长度 */
} ymodem_receiver_buffer_t;

/** @brief 接收器回调函数集合 */
typedef struct {
    ymodem_receiver_event_callback_t event_callback;         /**< 事件通知回调 */
    void*                            event_user_ctx;         /**< 事件回调用户上下文 */
    ymodem_receiver_send_response_t  send_response;          /**< 应答发送回调 */
    void*                            send_response_user_ctx; /**< 应答发送用户上下文 */
} ymodem_receiver_callbacks_t;

/** @brief 接收器协议处理状态 */
typedef struct {
    bool     is_handshake_active; /**< 握手是否处于活跃状态 */
    uint32_t handshake_count;     /**< 握手重试次数 */
    uint32_t last_time_ms;        /**< 上次活动时间戳（毫秒），用于超时计算 */
} ymodem_receiver_process_t;

/** @brief 接收器用户事件类型 */
typedef enum {
    YMODEM_RECV_EVENT_FILE_INFO,          /**< 收到文件信息包，用户应创建/打开文件 */
    YMODEM_RECV_EVENT_DATA_PACKET,        /**< 收到数据包，用户应将数据写入文件 */
    YMODEM_RECV_EVENT_TRANSFER_COMPLETE,  /**< 当前文件传输完成，用户应关闭文件 */
    YMODEM_RECV_EVENT_TRANSFER_FINISHED,  /**< 会话结束，解析器回到 IDLE */
    YMODEM_RECV_EVENT_ERROR,              /**< 传输错误/取消 */
} ymodem_receiver_event_type_t;

/** @brief 接收器事件详情 */
struct ymodem_receiver_event {
    ymodem_receiver_event_type_t type;      /**< 事件类型 */
    const char*   file_name;                /**< 文件名（FILE_INFO 有效） */
    uint32_t      file_size;                /**< 文件总大小（FILE_INFO 有效） */
    const uint8_t* data;                    /**< 数据指针（DATA_PACKET 有效，回调返回前有效） */
    uint32_t      data_seq;                 /**< 数据包序号（DATA_PACKET 有效） */
    uint32_t      data_len;                 /**< 数据有效长度（DATA_PACKET 有效） */
    uint32_t      total_received;           /**< 累计已接收字节数 */
};

/** @brief Ymodem 接收器主结构体 */
struct ymodem_receiver_parser
{
    ymodem_receiver_file_info_t      file_info;  /**< 文件信息 */
    ymodem_receiver_frame_info_t     frame_info; /**< 帧解析信息 */
    ymodem_receiver_buffer_t         buffer;     /**< 接收/发送缓冲区 */
    ymodem_receiver_callbacks_t      callbacks;  /**< 回调函数集合 */
    ymodem_receiver_file_parse_stat_e stat;      /**< 字节级解析状态 */
    ymodem_error_e                   error;      /**< 当前错误码 */
    ymodem_stage_e                   stage;      /**< 协议阶段 */
    ymodem_receiver_process_t        process;    /**< 协议处理状态 */
    ymodem_receiver_event_t          user_evt;   /**< 用户通知事件缓存 */
};
/**
 * @brief 创建 Ymodem 协议接收器
 *
 * @param parser         未初始化的解析器实例
 * @param rx_buffer      接收缓冲区（至少 YMODEM_STX_FRAME_LEN_BYTE 字节）
 * @param rx_buffer_size 接收缓冲区大小
 * @return true 创建成功
 * @return false 参数非法
 */
bool ymodem_receiver_create(ymodem_receiver_parser_t* parser, uint8_t* rx_buffer, uint32_t rx_buffer_size);

/**
 * @brief 设置事件通知回调
 *
 * @param parser   解析器实例
 * @param callback 事件回调函数
 * @param user_ctx 用户上下文（透传至回调）
 * @return true 成功
 * @return false parser 为 NULL
 */
bool ymodem_receiver_set_event_callback(ymodem_receiver_parser_t* parser, ymodem_receiver_event_callback_t callback, void* user_ctx);

/**
 * @brief 设置应答发送回调
 *
 * @param parser           解析器实例
 * @param send_response_cb 应答发送回调函数
 * @param user_ctx         用户上下文（透传至回调）
 * @return true 成功
 * @return false parser 为 NULL
 */
bool ymodem_receiver_set_send_response_callback(ymodem_receiver_parser_t* parser, ymodem_receiver_send_response_t send_response_cb, void* user_ctx);

/**
 * @brief Ymodem 接收器字节流解析入口
 *
 * 非阻塞式。逐字节解析输入数据，驱动内部状态机。
 * 帧处理完成后通过事件回调通知用户，应答数据通过 send_response 回调发出。
 *
 * @param parser 解析器实例
 * @param data   输入数据缓冲区
 * @param len    输入数据长度
 * @return ymodem_error_e 本次调用的处理结果：
 *         - ::YMODEM_ERROR_NONE    本次调用内完整接收了一帧
 *         - ::YMODEM_ERROR_WAIT_MORE  数据不足，需要继续喂入更多字节
 *         - ::YMODEM_ERROR_GARBAGE   帧间收到非帧头字节（非 Ymodem 数据，责任链应解锁）
 *         - 其他错误码              帧校验/序号/超限等错误
 */
ymodem_error_e ymodem_receiver_parse(ymodem_receiver_parser_t* parser, const uint8_t* data, uint32_t len);

/**
 * @brief Ymodem 接收器超时轮询
 *
 * 需在主循环中定期调用。检测帧接收超时和握手超时。
 *
 * @param parser 解析器实例
 * @return true  触发了超时处理（发送了 NAK 或重发了 'C'）
 * @return false 未超时、IDLE 状态或 parser 为 NULL
 */
bool ymodem_receiver_poll(ymodem_receiver_parser_t* parser);

/**
 * @brief 启动 Ymodem 接收器
 *
 * 发送初始 'C' 字符进入 ESTABLISHING 阶段。
 *
 * @param parser 解析器实例
 * @return true  启动成功
 * @return false parser 为 NULL
 */
bool ymodem_receiver_start(ymodem_receiver_parser_t* parser);

/**
 * @brief 复位 Ymodem 接收器内部状态
 *
 * 根据当前错误类型和协议阶段执行分级复位。
 *
 * @param parser 解析器实例
 * @return true 复位成功
 * @return false parser 为 NULL
 */
bool ymodem_receiver_reset(ymodem_receiver_parser_t* parser);
