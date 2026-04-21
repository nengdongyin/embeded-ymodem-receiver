/**
 * @file ymodem_rev.h
 * @brief Ymodem协议解析器头文件
 *
 * 定义了Ymodem协议解析器的数据结构、枚举类型和API函数。
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
 /**
  * @brief Ymodem协议解析器句柄结构体
  */
typedef struct ymodem_protocol_parser ymodem_protocol_parser_t;
typedef struct ymodem_event ymodem_event_t;

/**
 * @brief 事件通知回调
 * @param parser   解析器实例
 * @param event    事件信息
 * @param user_ctx 用户上下文
 */
typedef void (*ymodem_event_callback_t)(ymodem_protocol_parser_t* parser,
	const ymodem_event_t* event,
	void* user_ctx);

/**
 * @brief 帧准备好回调函数类型
 *
 * 当一个完整的数据帧被解析完成后，会调用此回调函数。
 *
 * @param parser 指向协议解析器句柄的指针
 * @param user_ctx 用户上下文指针
 */
typedef void (*frame_ready_t)(ymodem_protocol_parser_t* parser, void* user_ctx);

/**
 * @brief 发送响应回调函数类型
 *
 * 当协议解析器需要发送响应（如ACK、NAK、C、CAN等）时，会调用此回调函数。
 *
 * @param parser 指向协议解析器句柄的指针
 * @param user_ctx 用户上下文指针
 */
typedef void (*send_response_t)(ymodem_protocol_parser_t* parser, void* user_ctx);

/** @brief 最大重传次数 */
#define YMODEM_RETRANSMISSION_MAX_COUNT						(20)
/** @brief 超时时间（毫秒） */
#define YMODEM_TIMEOUT_MS									(1000)

/**
 * @brief Ymodem协议解析器状态枚举
 */
typedef enum
{
	YMODEM_WAIT_HEAD = 0, /**< 等待帧头 */
	YMODEM_WAIT_SEQ,      /**< 等待序号 */
	YMODEM_WAIT_DATA,     /**< 等待数据 */
	YMODEM_WAIT_CRC,      /**< 等待CRC */
	YMODEM_WAIT_CAN_2,    /**< 等待第二个CAN字符 */
}ymodem_file_prase_stat_e;

/**
 * @brief Ymodem协议处理状态枚举
 */
typedef enum
{
	YMODEM_START_HANDSHAKE = 0, /**< 开始握手 */
	YMODEM_WAIT_HANDSHAKE_ACK,  /**< 等待握手ACK */
}ymodem_process_stat_e;

/**
 * @brief Ymodem协议错误码枚举
 */
typedef enum
{
	YMODEM_ERROR_NONE = 0,              /**< 无错误 */
	YMODEM_ERROR_NEED_MORE_DATA,        /**< 需要更多数据 */
	YMODEM_ERROR_TIME_OUT,              /**< 超时错误 */
	YMODEM_ERROR_CRC,                   /**< CRC校验错误 */
	YMODEM_ERROR_SEQ,                   /**< 序号错误 */
	YMODEM_ERROR_HANDSHAKE_NACK,        /**< 握手NAK错误 */
	YMODEM_ERROR_RETRANSMISSION_COUNT_MAX, /**< 重传次数达到上限 */
	YMODEM_ERROR_SENDER_NO_REV_ACK,     /**< 发送方未收到ACK */
}ymodem_error_e;

/**
 * @brief Ymodem协议帧头字符枚举
 */
typedef enum
{
	YMODEM_SOH = 0x01, /**< 128字节数据帧头 */
	YMODEM_STX = 0x02, /**< 1024字节数据帧头 */
	YMODEM_EOT = 0x04, /**< 传输结束 */
	YMODEM_ACK = 0x06, /**< 确认 */
	YMODEM_NAK = 0x15, /**< 否定确认 */
	YMODEM_CAN = 0x18, /**< 取消 */
	YMODEM_C = 0x43,   /**< 请求使用CRC模式 */
}ymodem_head_e;

/**
 * @brief Ymodem帧类型枚举
 */
typedef enum
{
	YMODEM_FRAME_TYPE_SOH = 0x00, /**< SOH帧 */
	YMODEM_FRAME_TYPE_STX,        /**< STX帧 */
	YMODEM_FRAME_TYPE_EOT,        /**< EOT帧 */
	YMODEM_FRAME_TYPE_CAN,        /**< CAN帧 */
	YMODEM_FRAME_TYPE_NONE,       /**< 无帧类型 */
}ymodem_frame_type_e;

/**
 * @brief Ymodem协议阶段枚举
 */
typedef enum
{
	YMODEM_STAGE_IDLE = 0x00,      /**< 空闲阶段 */
	YMODEM_STAGE_ESTABLISHING,     /**< 建立连接阶段 */
	YMODEM_STAGE_ESTABLISHED,      /**< 连接已建立阶段 */
	YMODEM_STAGE_TRANSFERRING,     /**< 数据传输阶段 */
	YMODEM_STAGE_FINISHING,        /**< 完成传输阶段 */
	YMODEM_STAGE_FINISHED,         /**< 传输完成阶段 */
	YMODEM_STAGE_ABORTED,          /**< 传输被取消阶段 */
}ymodem_stage_e;

/**
 * @brief 文件信息结构体
 */
typedef struct
{
	bool  file_is_active;          /**< 文件是否激活 */
	char  file_name[128];          /**< 文件名 */
	uint32_t  file_total_size;     /**< 文件总大小 */
	uint32_t  file_rev_size;       /**< 已接收文件大小 */
	uint32_t  file_rev_frame_number; /**< 已接收帧数量 */
}ymodem_file_info_t;

/**
 * @brief 帧信息结构体
 */
typedef struct
{
	bool  frame_is_start;              /**< 帧是否开始 */
	bool  frame_is_end;                /**< 帧是否结束 */
	bool  current_frame_is_resend;     /**< 当前帧是否为重传 */
	ymodem_frame_type_e  frame_type;   /**< 帧类型 */
	uint32_t  current_frame_index;     /**< 当前帧索引 */
	uint32_t  current_frame_total_len; /**< 当前帧总长度 */
	uint32_t  current_frame_data_len;  /**< 当前帧数据长度 */
	uint32_t  current_frame_rev_len;   /**< 当前帧已接收长度 */
	uint32_t  current_frame_error_count; /**< 当前帧错误计数 */
}ymodem_frame_info_t;

/**
 * @brief 缓冲区结构体
 */
typedef struct
{
	uint8_t* rx_buffer;          /**< 接收缓冲区指针 */
	uint32_t rx_buffer_len;      /**< 接收缓冲区长度 */
	uint8_t tx_buffer[4];        /**< 发送缓冲区 */
	uint32_t tx_buffer_len;      /**< 发送缓冲区长度 */
	uint32_t tx_buffer_ack_len;  /**< 发送缓冲区中ACK数据长度 */
}ymodem_buffer_t;

/**
 * @brief 回调函数集合结构体
 */
typedef struct {
	ymodem_event_callback_t event_callback; /**< 事件通知回调 */
	void* event_user_ctx;                   /**< 事件回调用户上下文 */
	send_response_t send_response; /**< 发送响应回调函数 */
	void* send_response_user_ctx;                /**< 用户上下文指针 */
} parser_callbacks_t;

/**
 * @brief 协议处理信息结构体
 */
typedef struct {
	bool is_handshake_active;    /**< 握手是否活跃 */
	uint32_t handshake_count;    /**< 握手计数 */
	uint32_t last_time_ms;       /**< 上次活动时间（毫秒） */
} ymodem_process_t;

/**
 * @brief Ymodem 传输事件类型
 */
typedef enum {
	YMODEM_EVENT_FILE_INFO,			/**< 收到文件信息包（文件名、大小），用户应打开文件 */
	YMODEM_EVENT_DATA_PACKET,		/**< 收到数据包，用户应写入数据 */
	YMODEM_EVENT_TRANSFER_COMPLETE, /**< 传输完成（收到第二个 EOT 后的 ACK+C 已发送），用户应关闭文件 */
	YMODEM_EVENT_SESSION_FINISHED,  /**< 会话结束（收到空文件名包并应答 ACK），解析器回到 IDLE */
	YMODEM_EVENT_ERROR,				/**< 发生错误，传输中止 */
} ymodem_event_type_t;

/**
 * @brief Ymodem 事件信息
 */
struct ymodem_event {
	ymodem_event_type_t type;           /**< 事件类型 */
	const char* file_name;              /**< 文件名（仅对 FILE_INFO 有效） */
	uint32_t file_size;                 /**< 文件总大小（仅对 FILE_INFO 有效） */
	const uint8_t* data;                /**< 数据指针（仅对 DATA_PACKET 有效） */
	uint32_t data_seq;                  /**< 数据序号（仅对 DATA_PACKET 有效） */
	uint32_t data_len;                  /**< 数据长度（仅对 DATA_PACKET 有效） */
	uint32_t total_received;            /**< 当前已接收总字节数（对 DATA_PACKET 和 COMPLETE 有效） */
};

/**
 * @brief Ymodem协议解析器主结构体
 */
struct ymodem_protocol_parser
{
	ymodem_file_info_t file_info;     /**< 文件信息 */
	ymodem_frame_info_t frame_info;   /**< 帧信息 */
	ymodem_buffer_t buffer;           /**< 缓冲区 */
	parser_callbacks_t callbacks;     /**< 回调函数集合 */
	ymodem_file_prase_stat_e stat;    /**< 解析器状态 */
	ymodem_error_e error;             /**< 错误码 */
	ymodem_stage_e stage;             /**< 协议阶段 */
	ymodem_process_t process;         /**< 协议处理信息 */
	ymodem_event_t user_evt;		  /**<用户通知事件*/
};
/**
 * @brief 创建Ymodem协议解析器
 *
 * 初始化协议解析器句柄的基本信息。
 *
 * @param parser 指向协议解析器句柄的指针
 * @param rx_buffer 接收缓冲区指针
 * @param rx_buffer_size 接收缓冲区大小
 * @return true 成功，false 失败
 */
bool protocol_parser_create(ymodem_protocol_parser_t* parser, uint8_t* rx_buffer, uint32_t rx_buffer_size);

/**
 * @brief 设置事件通知回调
 * @param parser   解析器实例
 * @param callback 回调函数
 * @param user_ctx 用户上下文
 */
bool ymodem_set_event_callback(ymodem_protocol_parser_t* parser, ymodem_event_callback_t callback, void* user_ctx);

/**
 * @brief 设置发送响应回调函数
 *
 * @param parser 指向协议解析器句柄的指针
 * @param send_response_cb 发送响应回调函数
 * @param user_ctx 用户上下文指针
 * @return true 成功，false 失败
 */
bool protocol_parser_set_send_response_callback(ymodem_protocol_parser_t* parser, send_response_t send_response_cb, void* user_ctx);

/**
 * @brief Ymodem协议解析主函数
 *
 * 非阻塞式解析输入的数据流。
 *
 * @param parser 指向协议解析器句柄的指针
 * @param data 输入数据指针
 * @param len 输入数据长度
 */
void  ymodem_protocol_parser(ymodem_protocol_parser_t* parser, uint8_t* data, uint32_t len);

/**
 * @brief Ymodem协议超时处理函数
 *
 * 需要定期调用此函数来处理超时。
 *
 * @param parser 指向协议解析器句柄的指针
 */
void ymodem_protocol_process_poll(ymodem_protocol_parser_t* parser);

/**
 * @brief Ymodem协议解析器启动函数
 *
 * 需要使用该函数启动ymodem解析器发送起始C。
 *
 * @param parser 指向协议解析器句柄的指针
 */
void ymodem_protocol_start(ymodem_protocol_parser_t* parser);

/**
 * @brief 获取系统时间（毫秒）
 *
 * 此函数需要用户实现。
 *
 * @return 当前系统时间（毫秒）
 */
uint32_t system_get_time_ms(void);
