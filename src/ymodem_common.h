/**
 * @file ymodem_common.h
 * @brief Ymodem协议公共定义
 *
 * 包含Ymodem协议收发双方共用的帧结构宏定义、协议枚举类型、
 * CRC16查表计算接口以及系统时间接口。
 * 由 ymodem_receiver.h 和 ymodem_sender.h 共同引用。
 *
 * @note 帧结构布局 (以 SOH 帧为例):
 * @code
 *   [0]        [1]    [2]      [3..130]     [131]    [132]
 *   frame_type  seq   ~seq      data(128)    crc_hi   crc_lo
 * @endcode
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>

/* ================================================================
 *  帧结构宏定义
 * ================================================================ */

/** @brief 帧头长度（字节）: frame_type + seq + ~seq */
#define YMODEM_HEAD_LEN_BYTE                       (3)
/** @brief CRC16 校验码长度（字节） */
#define YMODEM_CRC_LEN_BYTE                        (2)
/** @brief SOH 帧数据区容量（字节） */
#define YMODEM_SOH_DATA_LEN_BYTE                   (128)
/** @brief STX 帧数据区容量（字节） */
#define YMODEM_STX_DATA_LEN_BYTE                   (1024)
/** @brief SOH 帧总长度（字节）: 3 + 128 + 2 = 133 */
#define YMODEM_SOH_FRAME_LEN_BYTE                  (YMODEM_HEAD_LEN_BYTE + YMODEM_SOH_DATA_LEN_BYTE + YMODEM_CRC_LEN_BYTE)
/** @brief STX 帧总长度（字节）: 3 + 1024 + 2 = 1029 */
#define YMODEM_STX_FRAME_LEN_BYTE                  (YMODEM_HEAD_LEN_BYTE + YMODEM_STX_DATA_LEN_BYTE + YMODEM_CRC_LEN_BYTE)
/** @brief 帧类型字节在帧内的索引（固定为 0） */
#define YMODEM_FRAME_TYPE_BYTE_INDEX               (0)
/** @brief 序号字节在帧内的索引 */
#define YMODEM_SEQ_BYTE_INDEX                      (1)
/** @brief 序号反码字节在帧内的索引 */
#define YMODEM_NOR_SEQ_BYTE_INDEX                  (2)
/** @brief 数据区在帧内的起始索引 */
#define YMODEM_DATA_BYTE_INDEX                     (3)
/** @brief 单帧最大重传次数上限 */
#define YMODEM_RETRANSMISSION_MAX_COUNT            (20)
/** @brief 帧超时时间（毫秒），用于 poll 判定 */
#define YMODEM_TIMEOUT_MS                          (1000)

/* ================================================================
 *  协议字符 / 帧类型 / 阶段 / 错误码 枚举
 * ================================================================ */

/**
 * @brief Ymodem 协议控制字符枚举
 *
 * 定义 Ymodem 链路层控制字符，用于帧头识别及应答。
 */
typedef enum
{
    YMODEM_SOH = 0x01, /**< 128 字节数据帧帧头 */
    YMODEM_STX = 0x02, /**< 1024 字节数据帧帧头 */
    YMODEM_EOT = 0x04, /**< 传输结束标记 */
    YMODEM_ACK = 0x06, /**< 肯定确认 */
    YMODEM_NAK = 0x15, /**< 否定确认 / 请求重传 */
    YMODEM_CAN = 0x18, /**< 取消传输 */
    YMODEM_C   = 0x43, /**< 请求使用 CRC16 校验模式 */
} ymodem_head_e;

/**
 * @brief Ymodem 帧类型枚举
 *
 * 内部状态机使用的帧类型标识，在解析帧头后赋值。
 */
typedef enum
{
    YMODEM_FRAME_TYPE_SOH  = 0x00, /**< 128 字节数据帧 */
    YMODEM_FRAME_TYPE_STX,         /**< 1024 字节数据帧 */
    YMODEM_FRAME_TYPE_EOT,         /**< 传输结束帧 */
    YMODEM_FRAME_TYPE_CAN,         /**< 取消传输帧 */
    YMODEM_FRAME_TYPE_NONE,        /**< 无效帧类型（初始值） */
} ymodem_frame_type_e;

/**
 * @brief Ymodem 协议阶段枚举
 *
 * 描述收发双方协议状态机所处的宏观阶段。
 * 阶段转换由 frame_stage_process() 驱动。
 */
typedef enum
{
    YMODEM_STAGE_IDLE         = 0x00, /**< 空闲，未启动传输 */
    YMODEM_STAGE_ESTABLISHING,        /**< 连接建立中（握手阶段） */
    YMODEM_STAGE_ESTABLISHED,         /**< 连接已建立，等待首个数据帧 */
    YMODEM_STAGE_TRANSFERRING,        /**< 数据传输进行中 */
    YMODEM_STAGE_FINISHING,           /**< 文件传输收尾中（已发/收首个 EOT） */
    YMODEM_STAGE_FINISHED,            /**< 文件传输完成（已发/收第二个 EOT） */
    YMODEM_STAGE_ABORTED,             /**< 传输被取消 / 异常中止 */
} ymodem_stage_e;

/**
 * @brief Ymodem 错误码枚举
 *
 * 包含收发双方可能出现的所有协议层错误类型。
 */
typedef enum
{
    YMODEM_ERROR_NONE                    = 0, /**< 无错误（帧处理完成） */
    YMODEM_ERROR_WAIT_MORE,                   /**< 等待更多数据（帧接收中途，非错误） */
    YMODEM_ERROR_GARBAGE,                     /**< 帧间收到非帧头字节（非 Ymodem 数据） */
    YMODEM_ERROR_TIME_OUT,                    /**< 超时错误（poll 触发） */
    YMODEM_ERROR_CRC,                         /**< CRC 校验失败 */
    YMODEM_ERROR_SEQ,                         /**< 序号不匹配 */
    YMODEM_ERROR_HANDSHAKE_NACK,              /**< 握手阶段收到非预期应答 */
    YMODEM_ERROR_RETRANSMISSION_COUNT_MAX,    /**< 重传次数达到上限 */
    YMODEM_ERROR_SENDER_NO_REV_ACK,           /**< 发送端未收到 ACK（重传帧场景） */
    YMODEM_ERROR_RESEND,                      /**< 请求重传当前帧 */
    YMODEM_ERROR_CAN,                         /**< 收到 CAN 取消序列 */
} ymodem_error_e;

/* ================================================================
 *  公共函数声明
 * ================================================================ */

/**
 * @brief 使用查表法计算 CRC16 (XMODEM CCITT 标准, init=0)
 *
 * 对指定数据区计算 CRC16 校验值，用于帧的发送/验证。
 *
 * @param data 数据区起始指针
 * @param size 数据区长度（字节）
 * @return uint16_t CRC16 校验值
 */
uint16_t ymodem_calculate_crc16(const uint8_t* data, uint32_t size);

/**
 * @brief 获取系统毫秒级时间戳
 *
 * 该函数**必须**由应用层实现。库不提供默认实现，
 * 链接时将因符号未定义而失败。
 *
 * @return uint32_t 当前系统时间（毫秒）
 */
uint32_t system_get_time_ms(void);
