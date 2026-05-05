/**
 * @file ymodem_receiver.c
 * @brief Ymodem 协议接收器实现
 *
 * 实现 Ymodem 协议接收端的状态机、帧解析和应答逻辑。
 *
 * @par 协议状态流转（概览）
 * @code
 *   create() → IDLE → start() → 发送 'C' → ESTABLISHING
 *     → 收 SOH(seq=0) → FILE_INFO 解析 → ACK+C → ESTABLISHED
 *       → 收 第一数据帧(seq=1) → DATA_PACKET 回调 → ACK → TRANSFERRING
 *         → 收后续数据帧 → DATA_PACKET 回调 → ACK ...
 *         → 收 EOT → NAK → FINISHING
 *           → 收 EOT → ACK+C → TRANSFER_COMPLETE → FINISHED
 *             → 收 SOH(seq=0) 新文件 → 循环
 *             → 收 空文件名帧 → TRANSFER_FINISHED → IDLE
 *   CAN(任意阶段) → ABORTED → IDLE
 * @endcode
 *
 * @par 延迟复位机制
 * frame_stage_process() 完成后仅设置 frame_is_end=1，
 * 真正的 ymodem_receiver_reset() 延迟到下次收到帧头字节时执行。
 * 这使得用户可以在 parse() 返回后读取完整帧信息。
 */

#include "ymodem_receiver.h"
#include <string.h>
#include <stdlib.h>

/* ================================================================
 *  内部辅助: 复位 / 应答构造 / 事件构造
 * ================================================================ */

/**
 * @brief 复位接收器帧级状态
 *
 * 根据当前错误类型和协议阶段执行分级复位：
 * - 正常帧 (NONE): 清 frame_error_count，标记握手活跃
 * - 超时/CRC/SEQ 错误: 仅通过通用复位清 frame_info，不清 error_count
 * - 重传超限 (RETRANSMISSION_MAX): 清 process 和 file_info
 *
 * @param parser 解析器实例
 * @return true 成功
 * @return false parser 为 NULL
 */
bool ymodem_receiver_reset(ymodem_receiver_parser_t* parser)
{
    if (!parser) {
        return false;
    }

    /* ---- 阶段复位（按当前错误类型分级） ---- */
    if (parser->error == YMODEM_ERROR_NONE) {
        parser->frame_info.current_frame_error_count = 0;
        parser->process.is_handshake_active = true;
        parser->process.handshake_count = 0;
        switch (parser->stage)
        {
        case YMODEM_STAGE_FINISHED:
        case YMODEM_STAGE_IDLE: {
            memset(&parser->file_info, 0, sizeof(parser->file_info));
            break;
        }
        default:
            break;
        }
    }
    else {
        switch (parser->error)
        {
        case YMODEM_ERROR_TIME_OUT:
        case YMODEM_ERROR_CRC:
        case YMODEM_ERROR_SEQ: {
            break;
        }
        /* 重传超限: 完全清理，准备重新握手 */
        case YMODEM_ERROR_RETRANSMISSION_COUNT_MAX: {
            memset(&parser->process, 0, sizeof(parser->process));
            memset(&parser->file_info, 0, sizeof(parser->file_info));
            parser->process.is_handshake_active = false;
            parser->process.handshake_count = 0;
            break;
        }
        default:
            break;
        }

    }
    /* ---- 通用复位: 清 frame_info，保留 error_count ---- */
    uint32_t saved_error_count = parser->frame_info.current_frame_error_count;
    memset(&parser->frame_info, 0, sizeof(parser->frame_info));
    parser->frame_info.current_frame_error_count = saved_error_count;
    if ((parser->stage != YMODEM_STAGE_FINISHING) && (parser->stage != YMODEM_STAGE_IDLE)) {
        parser->stat = YMODEM_RECV_WAIT_HEAD;
    }
    parser->error = YMODEM_ERROR_NONE;
    return true;
}

/**
 * @brief 构造用户事件结构体
 *
 * @param parser   解析器实例
 * @param evt_type 事件类型
 * @return true 成功
 * @return false parser 为 NULL
 */
static bool frame_user_event_set(ymodem_receiver_parser_t* parser, ymodem_receiver_event_type_t evt_type)
{
    if (!parser) {
        return false;
    }
    parser->user_evt.type = evt_type;
    parser->user_evt.file_name = parser->file_info.file_name;
    parser->user_evt.file_size = parser->file_info.file_total_size;
    parser->user_evt.data = &parser->buffer.rx_buffer[YMODEM_DATA_BYTE_INDEX];
    parser->user_evt.data_seq = parser->file_info.file_rev_frame_number > 1 ? parser->file_info.file_rev_frame_number - 1 : 0;
    parser->user_evt.data_len = parser->frame_info.current_frame_data_len;
    parser->user_evt.total_received = parser->file_info.file_rev_size;
    return true;
}

/* ================================================================
 *  应答编码
 * ================================================================ */

/**
 * @brief 编码 NAK 应答
 */
static bool frame_nak_without_data(ymodem_receiver_parser_t* parser)
{
    if (!parser) {
        return false;
    }
    parser->buffer.tx_buffer_ack_len = 1;
    parser->buffer.tx_buffer[0] = YMODEM_NAK;
    return true;
}

/**
 * @brief 编码 ACK 应答
 */
static bool frame_ack_without_data(ymodem_receiver_parser_t* parser)
{
    if (!parser) {
        return false;
    }
    parser->buffer.tx_buffer_ack_len = 1;
    parser->buffer.tx_buffer[0] = YMODEM_ACK;
    return true;
}

/**
 * @brief 编码 ACK + C 组合应答（确认文件信息包或传输完成）
 */
static bool frame_ack_c_without_data(ymodem_receiver_parser_t* parser)
{
    if (!parser) {
        return false;
    }
    parser->buffer.tx_buffer_ack_len = 2;
    parser->buffer.tx_buffer[0] = YMODEM_ACK;
    parser->buffer.tx_buffer[1] = YMODEM_C;
    return true;
}

/**
 * @brief 编码 ACK + C 应答，并触发用户事件回调
 */
static bool frame_ack_c_with_data(ymodem_receiver_parser_t* parser, ymodem_receiver_event_type_t evt_type)  
{
    if (!parser) {
        return false;
    }
    frame_ack_c_without_data(parser);
    frame_user_event_set(parser, evt_type);
    if (parser->callbacks.event_callback) {
        parser->callbacks.event_callback(parser, &parser->user_evt, parser->callbacks.event_user_ctx);
    }
    return true;
}

/**
 * @brief 编码 ACK 应答，并触发用户事件回调
 */
static bool frame_ack_with_data(ymodem_receiver_parser_t* parser, ymodem_receiver_event_type_t evt_type)
{
    if (!parser) {
        return false;
    }
    frame_ack_without_data(parser);
    frame_user_event_set(parser, evt_type);
    if (parser->callbacks.event_callback) {
        parser->callbacks.event_callback(parser, &parser->user_evt, parser->callbacks.event_user_ctx);
    }
    return true;
}

/* ================================================================
 *  文件信息解析
 * ================================================================ */

/**
 * @brief 从 rx_buffer 的数据区解析文件名和文件大小
 *
 * 格式: filename\0size_decimal_string
 *
 * @param parser 解析器实例
 * @return true 解析成功
 * @return false 解析失败（parser 为 NULL 或格式非法）
 */
static bool ymodem_receiver_file_info_parse(ymodem_receiver_parser_t* parser)
{
    if (!parser) {
        return false;
    }
    uint8_t* data_ptr = &parser->buffer.rx_buffer[YMODEM_DATA_BYTE_INDEX];

    /* 解析文件名（直到 '\0'） */
    uint32_t i = 0;
    while (i < (sizeof(parser->file_info.file_name) - 1) && data_ptr[i] != '\0') {
        parser->file_info.file_name[i] = data_ptr[i];
        i++;
    }
    parser->file_info.file_name[i] = '\0';

    /* 跳过 '\0' 及非数字字符，找到文件大小起始位置 */
    while (i < YMODEM_SOH_DATA_LEN_BYTE && (data_ptr[i] < '0' || data_ptr[i] > '9')) {
        i++;
    }
    data_ptr[YMODEM_SOH_DATA_LEN_BYTE - 1] = '\0';
    if (i < YMODEM_SOH_DATA_LEN_BYTE) {
        parser->file_info.file_total_size = strtoul((char*)&data_ptr[i], NULL, 10);
        return true;
    }
    return false;
}

/* ================================================================
 *  错误处理
 * ================================================================ */

/**
 * @brief 根据错误类型编码应答数据
 *
 * - TIME_OUT/CRC/SEQ → NAK
 * - HANDSHAKE_NACK → C
 * - RETRANSMISSION_MAX → CAN
 * - SENDER_NO_REV_ACK → ACK
 *
 * @param parser 解析器实例
 * @return true 编码成功
 * @return false 未知错误类型
 */
static bool  frame_error_response_encode(ymodem_receiver_parser_t* parser)
{
    if (!parser) {
        return false;
    }
    switch (parser->error) {
    case YMODEM_ERROR_TIME_OUT:
    case YMODEM_ERROR_CRC:
    case YMODEM_ERROR_SEQ: {
        parser->buffer.tx_buffer_ack_len = 1;
        parser->buffer.tx_buffer[0] = YMODEM_NAK;
        break;
    }
    case YMODEM_ERROR_HANDSHAKE_NACK: {
        parser->buffer.tx_buffer_ack_len = 1;
        parser->buffer.tx_buffer[0] = YMODEM_C;
        break;
    }
    /* 重传超限: 发 CAN 并通知用户 */
    case YMODEM_ERROR_RETRANSMISSION_COUNT_MAX: {
        parser->buffer.tx_buffer_ack_len = 2;
        parser->buffer.tx_buffer[0] = YMODEM_CAN;
        parser->buffer.tx_buffer[1] = YMODEM_CAN;
        frame_user_event_set(parser, YMODEM_RECV_EVENT_ERROR);
        if (parser->callbacks.event_callback) {
            parser->callbacks.event_callback(parser, &parser->user_evt, parser->callbacks.event_user_ctx);
        }
        break;
    }
    /* 发送端重发旧帧: 应答 ACK（已丢弃重复数据） */
    case YMODEM_ERROR_SENDER_NO_REV_ACK: {
        parser->buffer.tx_buffer_ack_len = 1;
        parser->buffer.tx_buffer[0] = YMODEM_ACK;
        break;
    }
    default:
        return false;
    }
    return true;
}

/**
 * @brief 处理错误并更新重传计数
 *
 * 递增对应错误计数器，超限则升级为 RETRANSMISSION_COUNT_MAX 并中止传输。
 *
 * @param parser 解析器实例
 * @return true 已处理
 * @return false 未知错误
 */
static bool frame_error_process(ymodem_receiver_parser_t* parser)
{
    if (!parser) {
        return false;
    }
    switch (parser->error) {
    case YMODEM_ERROR_TIME_OUT:
    case YMODEM_ERROR_CRC:
    case YMODEM_ERROR_SEQ: {
        parser->frame_info.current_frame_error_count++;
        if (parser->frame_info.current_frame_error_count > YMODEM_RETRANSMISSION_MAX_COUNT) {
            parser->error = YMODEM_ERROR_RETRANSMISSION_COUNT_MAX;
            parser->stage = YMODEM_STAGE_IDLE;
        }
        return true;
    }
    case YMODEM_ERROR_HANDSHAKE_NACK: {
        parser->process.handshake_count++;
        if (parser->process.handshake_count > YMODEM_RETRANSMISSION_MAX_COUNT) {
            parser->error = YMODEM_ERROR_RETRANSMISSION_COUNT_MAX;
            parser->stage = YMODEM_STAGE_IDLE;
        }
        return true;
    }
    case YMODEM_ERROR_SENDER_NO_REV_ACK: {
        return true;
    }
    default:
        return false;
    }
}

/* ================================================================
 *  协议阶段状态机 — frame_stage_process()
 * ================================================================ */

/**
 * @brief 接收端协议阶段处理主状态机
 *
 * 根据当前 parser->stage 和 parser->error 执行帧处理/应答/阶段切换。
 * 处理后设置 frame_is_end = true，复位延迟到下一帧头。
 *
 * @par 阶段流转图
 * @code
 *   IDLE ──start()──→ ESTABLISHING ──SOH(seq=0)──→ ESTABLISHED
 *                          │                           │
 *                          │                    (数据帧 seq=1)
 *                          │                           ↓
 *                          │                    TRANSFERRING ←──┐
 *                          │                      │  │          │
 *                          │              (数据帧) │  └── EOT ──┤
 *                          │                      ↓            │
 *                          │                    FINISHING      │
 *                          │                      │ (EOT)      │
 *                          │                      ↓            │
 *                          │                    FINISHED ──────┘
 *                          │                      │
 *                          │     (SOH seq=0 新文件)│
 *                          │                      ↓
 *                          │                    ESTABLISHED (下一文件)
 *                          │
 *   CAN ──→ ABORTED ──→ IDLE
 * @endcode
 *
 * @param parser 解析器实例
 */
static void frame_stage_process(ymodem_receiver_parser_t* parser)
{
    if (!parser) {
        return;
    }

    /* ================================================================
     * 分支 1: 错误处理
     * ================================================================ */
    if (parser->error != YMODEM_ERROR_NONE) {
        frame_error_process(parser);
        frame_error_response_encode(parser);
    }
    /* ================================================================
     * 分支 2: 正常帧处理（按 stage 分发）
     * ================================================================ */
    else {
        /* CAN 在任何阶段无条件取消 */
        if (parser->frame_info.frame_type == YMODEM_FRAME_TYPE_CAN) {
            parser->stage = YMODEM_STAGE_IDLE;
            frame_ack_with_data(parser, YMODEM_RECV_EVENT_ERROR);
        }
        else {
            switch (parser->stage)
            {
            /* --------------------------------------------------------
             * IDLE: 空闲，不处理
             * -------------------------------------------------------- */
            case YMODEM_STAGE_IDLE: {
                break;
            }

            /* --------------------------------------------------------
             * ESTABLISHING: 等待文件信息帧 (SOH, seq=0)
             * 成功后发送 ACK+C 进入 ESTABLISHED
             * -------------------------------------------------------- */
            case YMODEM_STAGE_ESTABLISHING: {
                if ((parser->frame_info.frame_type == YMODEM_FRAME_TYPE_SOH) && (parser->frame_info.current_frame_index == 0)) {
                    if (ymodem_receiver_file_info_parse(parser) == true) {
                        parser->stage = YMODEM_STAGE_ESTABLISHED;
                        parser->file_info.file_rev_frame_number++;
                        frame_ack_c_with_data(parser, YMODEM_RECV_EVENT_FILE_INFO);
                    }
                }
                break;
            }

            /* --------------------------------------------------------
             * ESTABLISHED: 处理第一数据帧 (seq=1) 或 EOT
             * 数据帧 → ACK → 切换为 TRANSFERRING
             * -------------------------------------------------------- */
            case YMODEM_STAGE_ESTABLISHED: {
                if (((parser->frame_info.frame_type == YMODEM_FRAME_TYPE_STX || parser->frame_info.frame_type == YMODEM_FRAME_TYPE_SOH))
                    && (parser->frame_info.current_frame_index == 1)) {
                    parser->file_info.file_rev_frame_number++;
                    uint32_t real_len = parser->frame_info.current_frame_data_len;
                    /* 最后一包截断：实际数据量不能超过文件剩余字节 */
                    if (parser->file_info.file_rev_size + real_len > parser->file_info.file_total_size) {
                        real_len = parser->file_info.file_total_size - parser->file_info.file_rev_size;
                    }
                    parser->frame_info.current_frame_data_len = real_len;
                    parser->file_info.file_rev_size += real_len;
                    parser->stage = YMODEM_STAGE_TRANSFERRING;
                    frame_ack_with_data(parser, YMODEM_RECV_EVENT_DATA_PACKET);
                }
                else if (parser->frame_info.frame_type == YMODEM_FRAME_TYPE_EOT) {
                    parser->stage = YMODEM_STAGE_FINISHING;
                    frame_nak_without_data(parser);
                }
                break;
            }

            /* --------------------------------------------------------
             * TRANSFERRING: 持续接收数据帧
             * 收完最后数据 → EOT → FINISHING
             * -------------------------------------------------------- */
            case YMODEM_STAGE_TRANSFERRING: {
                if ((parser->frame_info.frame_type == YMODEM_FRAME_TYPE_STX) || (parser->frame_info.frame_type == YMODEM_FRAME_TYPE_SOH)) {
                    parser->file_info.file_rev_frame_number++;
                    uint32_t real_len = parser->frame_info.current_frame_data_len;
                    if(parser->file_info.file_rev_size + real_len > parser->file_info.file_total_size){
                        real_len = parser->file_info.file_total_size - parser->file_info.file_rev_size;
                    }
                    parser->frame_info.current_frame_data_len = real_len;
                    parser->file_info.file_rev_size += real_len;
                    frame_ack_with_data(parser, YMODEM_RECV_EVENT_DATA_PACKET);
                }
                else if (parser->frame_info.frame_type == YMODEM_FRAME_TYPE_EOT) {
                    parser->stage = YMODEM_STAGE_FINISHING;
                    frame_nak_without_data(parser);
                }
                else {
                    ;
                }
                break;
            }

            /* --------------------------------------------------------
             * FINISHING: 等待第二个 EOT
             * 收到 → ACK+C → 通知 TRANSFER_COMPLETE
             * -------------------------------------------------------- */
            case YMODEM_STAGE_FINISHING: {
                if (parser->frame_info.frame_type == YMODEM_FRAME_TYPE_EOT) {
                    parser->stage = YMODEM_STAGE_FINISHED;
                    frame_ack_c_with_data(parser, YMODEM_RECV_EVENT_TRANSFER_COMPLETE);
                }
                else {
                    parser->error = YMODEM_ERROR_SEQ;
                    frame_nak_without_data(parser);
                }
                break;
            }

            /* --------------------------------------------------------
             * FINISHED: 等待下一文件信息帧 (SOH, seq=0)
             * 文件名非空 → ESTABLISHED（新文件）
             * 文件名为空 → IDLE（会话结束）
             * -------------------------------------------------------- */
            case YMODEM_STAGE_FINISHED: {
                if ((parser->frame_info.frame_type == YMODEM_FRAME_TYPE_SOH) && (parser->frame_info.current_frame_index == 0)) {
                    if (ymodem_receiver_file_info_parse(parser) == true) {
                        parser->stage = YMODEM_STAGE_ESTABLISHED;
                        frame_ack_c_with_data(parser, YMODEM_RECV_EVENT_FILE_INFO);
                    }
                    else {
                        parser->stage = YMODEM_STAGE_IDLE;
                        frame_ack_with_data(parser,YMODEM_RECV_EVENT_TRANSFER_FINISHED);
                    }
                }
                else {
                    frame_nak_without_data(parser);
                }
                break;
            }
            default: {
                frame_nak_without_data(parser);
                break;
            }
            }
        }
    }
    /* 触发应答发送回调 */
    if (parser->callbacks.send_response) {
        parser->callbacks.send_response(parser, parser->callbacks.send_response_user_ctx);
    }
    /* 标记帧处理完成，延迟到下一帧头触发复位 */
    parser->frame_info.frame_is_end = true;
    parser->frame_info.frame_is_start = false;
}

/* ================================================================
 *  公开 API
 * ================================================================ */

bool ymodem_receiver_create(ymodem_receiver_parser_t* parser, uint8_t* rx_buffer, uint32_t rx_buffer_size)
{
    if (!parser) {
        return false;
    }
    if (!rx_buffer) {
        return false;
    }
    if (rx_buffer_size < YMODEM_STX_FRAME_LEN_BYTE) {
        return false;
    }
    memset(parser, 0, sizeof(ymodem_receiver_parser_t));
    parser->buffer.rx_buffer = rx_buffer;
    parser->buffer.rx_buffer_len = rx_buffer_size;
    parser->buffer.tx_buffer_len = sizeof(parser->buffer.tx_buffer);
    parser->stat = YMODEM_RECV_WAIT_HEAD;
    parser->process.last_time_ms = system_get_time_ms();
    parser->stage = YMODEM_STAGE_IDLE;
    return true;
}

bool ymodem_receiver_set_event_callback(ymodem_receiver_parser_t* parser, ymodem_receiver_event_callback_t callback, void* user_ctx)
{
    if (!parser) {
        return false;
    }
    parser->callbacks.event_callback = callback;
    parser->callbacks.event_user_ctx = user_ctx;
    return true;
}

bool ymodem_receiver_set_send_response_callback(ymodem_receiver_parser_t* parser, ymodem_receiver_send_response_t send_response_cb, void* user_ctx)
{
    if (!parser) {
        return false;
    }
    parser->callbacks.send_response = send_response_cb;
    parser->callbacks.send_response_user_ctx = user_ctx;
    return true;
}

/**
 * @brief 接收器字节流解析入口
 *
 * 逐字节扫描，按 stat 两层状态机解析：
 * - WAIT_CAN_2:    等待第二个 CAN 字节
 * - WAIT_HEAD:     识别 SOH/STX/EOT/CAN 帧头
 * - WAIT_SEQ:      接收并验证序号 + 反码
 * - WAIT_DATA:     逐字节填充数据区
 * - WAIT_CRC:      接收并验证 CRC16
 *
 * frame_is_end 标志控制延迟复位：帧完成后跳过非帧头字节，
 * 等待下一帧头字节时才复位，保证用户的断言窗口。
 *
 * @return ymodem_error_e 本次调用结果：
 *         - YMODEM_ERROR_NONE    — 本次调用内完整处理了一帧（成功）
 *         - YMODEM_ERROR_WAIT_MORE — 未完成帧处理，需要继续喂入数据
 *         - YMODEM_ERROR_GARBAGE   — 帧间收到非帧头字节，非 Ymodem 数据
 *         - 其他                  — 帧错误码（CRC/SEQ/重传超限等）
 */
ymodem_error_e ymodem_receiver_parse(ymodem_receiver_parser_t* parser, const uint8_t* data, uint32_t len)
{
    if ((!parser) || (!data) || (!len)) {
        return YMODEM_ERROR_WAIT_MORE;
    }
    if (parser->stage == YMODEM_STAGE_IDLE) {
        return YMODEM_ERROR_GARBAGE;
    }
    bool had_pending = parser->frame_info.frame_is_end;
    bool saw_garbage = false;
    bool started_new_frame = false;

    for (uint32_t i = 0; i < len; i++) {
        uint8_t byte = data[i];

        /* ---- 延迟复位：前帧已完成，等帧头或标记垃圾字节 ---- */
        if (parser->frame_info.frame_is_end) {
            if (byte == YMODEM_SOH || byte == YMODEM_STX || byte == YMODEM_EOT || byte == YMODEM_CAN) {
                ymodem_receiver_reset(parser);
                started_new_frame = true;
            } else {
                saw_garbage = true;
                continue;
            }
        }

        /* ---- 缓冲区溢出保护 ---- */
        if (parser->frame_info.current_frame_rev_len >= parser->buffer.rx_buffer_len) {
            parser->error = YMODEM_ERROR_CRC;
            frame_stage_process(parser);
        }

        /* ---- 字节级状态机 ---- */
        switch (parser->stat)
        {
        /* WAIT_CAN_2: 确认是否为两字节 CAN 取消序列 */
        case YMODEM_RECV_WAIT_CAN_2: {
            if (byte == YMODEM_CAN) {
                parser->frame_info.frame_type = YMODEM_FRAME_TYPE_CAN;
                parser->error = YMODEM_ERROR_NONE;
                frame_stage_process(parser);
            }
            else {
                parser->stat = YMODEM_RECV_WAIT_HEAD;
                i--;
            }
            break;
        }

        /* WAIT_HEAD: 检测帧头字节并设置帧参数 */
        case YMODEM_RECV_WAIT_HEAD: {
            if (byte == YMODEM_SOH) {
                parser->frame_info.frame_type = YMODEM_FRAME_TYPE_SOH;
                parser->frame_info.current_frame_total_len = YMODEM_SOH_FRAME_LEN_BYTE;
                parser->frame_info.current_frame_data_len = YMODEM_SOH_DATA_LEN_BYTE;
                parser->buffer.rx_buffer[parser->frame_info.current_frame_rev_len++] = byte;
                parser->stat = YMODEM_RECV_WAIT_SEQ;
                parser->frame_info.frame_is_start = true;
                parser->process.is_handshake_active = true;
                parser->process.last_time_ms = system_get_time_ms();
            }
            else if (byte == YMODEM_STX) {
                parser->frame_info.frame_type = YMODEM_FRAME_TYPE_STX;
                parser->frame_info.current_frame_total_len = YMODEM_STX_FRAME_LEN_BYTE;
                parser->frame_info.current_frame_data_len = YMODEM_STX_DATA_LEN_BYTE;
                parser->buffer.rx_buffer[parser->frame_info.current_frame_rev_len++] = byte;
                parser->stat = YMODEM_RECV_WAIT_SEQ;
                parser->frame_info.frame_is_start = true;
                parser->process.is_handshake_active = true;
                parser->process.last_time_ms = system_get_time_ms();
            }
            /* EOT: 单字节帧，直接触发 stage 处理 */
            else if (byte == YMODEM_EOT) {
                parser->error = YMODEM_ERROR_NONE;
                parser->frame_info.frame_type = YMODEM_FRAME_TYPE_EOT;
                parser->stat = YMODEM_RECV_WAIT_HEAD;
                frame_stage_process(parser);
            }
            /* CAN: 第一个 CAN 字节，进入 WAIT_CAN_2 */
            else if (byte == YMODEM_CAN) {
                parser->frame_info.frame_type = YMODEM_FRAME_TYPE_CAN;
                parser->stat = YMODEM_RECV_WAIT_CAN_2;
                parser->frame_info.frame_is_start = true;
                parser->process.last_time_ms = system_get_time_ms();
            }
            else {
                ;
            }
            break;
        }

        /* WAIT_SEQ: 接收序号 + 反码，验证 seq == ~seq_n */
        case YMODEM_RECV_WAIT_SEQ: {
            parser->buffer.rx_buffer[parser->frame_info.current_frame_rev_len++] = byte;
            if (parser->frame_info.current_frame_rev_len == YMODEM_HEAD_LEN_BYTE) {
                uint8_t seq = parser->buffer.rx_buffer[YMODEM_SEQ_BYTE_INDEX];
                uint8_t seq_n = parser->buffer.rx_buffer[YMODEM_NOR_SEQ_BYTE_INDEX];
                /* 反码校验: seq + ~seq = 0xFF */
                if ((uint8_t)(seq + seq_n) == 0xFF) {
                    uint8_t expected_seq = parser->file_info.file_rev_frame_number & 0xFF;
                    uint8_t prev_seq = (expected_seq == 0) ? 0xFF : expected_seq - 1;
                    /* 正常帧 */
                    if (parser->buffer.rx_buffer[YMODEM_SEQ_BYTE_INDEX] == expected_seq) {
                        parser->stat = YMODEM_RECV_WAIT_DATA;
                    }
                    /* 重传帧（seq 为 prev） */
                    else if (parser->buffer.rx_buffer[YMODEM_SEQ_BYTE_INDEX] == prev_seq) {
                        parser->frame_info.current_frame_is_resend = true;
                        parser->stat = YMODEM_RECV_WAIT_DATA;
                    }
                    /* 序号不匹配 */
                    else {
                        parser->error = YMODEM_ERROR_SEQ;
                        frame_stage_process(parser);
                    }
                    parser->frame_info.current_frame_index = parser->buffer.rx_buffer[YMODEM_SEQ_BYTE_INDEX];
                }
                /* 反码校验失败 */
                else {
                    parser->error = YMODEM_ERROR_SEQ;
                    frame_stage_process(parser);
                }
            }
            break;
        }

        /* WAIT_DATA: 逐字节接收数据区，填满后进入 WAIT_CRC */
        case YMODEM_RECV_WAIT_DATA: {
            parser->buffer.rx_buffer[parser->frame_info.current_frame_rev_len++] = byte;
            if (parser->frame_info.current_frame_rev_len == parser->frame_info.current_frame_total_len - YMODEM_CRC_LEN_BYTE) {
                parser->stat = YMODEM_RECV_WAIT_CRC;
            }
            break;
        }

        /* WAIT_CRC: 接收两字节 CRC16 并校验 */
        case YMODEM_RECV_WAIT_CRC: {
            parser->buffer.rx_buffer[parser->frame_info.current_frame_rev_len++] = byte;
            if (parser->frame_info.current_frame_rev_len == parser->frame_info.current_frame_total_len) {
                uint16_t crc0_index = parser->frame_info.current_frame_total_len - 2;
                uint16_t crc1_index = parser->frame_info.current_frame_total_len - 1;
                uint16_t received_crc = (parser->buffer.rx_buffer[crc0_index] << 8) | parser->buffer.rx_buffer[crc1_index];
                uint16_t calculated_crc = ymodem_calculate_crc16(&parser->buffer.rx_buffer[YMODEM_DATA_BYTE_INDEX], parser->frame_info.current_frame_data_len);
                /* CRC 不匹配 → CRC 错误 */
                if (received_crc != calculated_crc) {
                    parser->error = YMODEM_ERROR_CRC;
                    frame_stage_process(parser);
                }
                else {
                    /* CRC 正确: 重传帧应答 ACK，正常帧应答 ACK+C */
                    if (parser->frame_info.current_frame_is_resend == true) {
                        parser->error = YMODEM_ERROR_SENDER_NO_REV_ACK;
                    }
                    else {
                        parser->error = YMODEM_ERROR_NONE;
                    }
                    frame_stage_process(parser);
                    parser->frame_info.frame_is_end = true;
                }
            }
            break;
        }

        default:
            break;
        }
    }

    if (parser->frame_info.frame_is_end && (!had_pending || started_new_frame)) {
        return parser->error;
    }
    if (saw_garbage) {
        return YMODEM_ERROR_GARBAGE;
    }
    return YMODEM_ERROR_WAIT_MORE;
}

/**
 * @brief 接收器超时轮询
 *
 * 检测两种超时场景：
 * - ESTABLISHING 阶段握手超时 → 重发 'C'
 * - 帧接收中途超时 (frame_is_start) → NAK
 *
 * @return true  触发了超时处理（已发送应答）
 * @return false 未超时、IDLE 状态或 parser 为 NULL
 */
bool ymodem_receiver_poll(ymodem_receiver_parser_t* parser)
{
    if (!parser) {
        return false;
    }
    if (parser->stage == YMODEM_STAGE_IDLE) {
        return false;
    }
    uint32_t now = system_get_time_ms();
    if (now - parser->process.last_time_ms < YMODEM_TIMEOUT_MS) {
        return false;
    }
    parser->process.last_time_ms = now;

    /* ESTABLISHING 握手超时 → 重发 'C' */
    if (parser->stage == YMODEM_STAGE_ESTABLISHING) {
        if (parser->process.is_handshake_active == false) {
            parser->error = YMODEM_ERROR_HANDSHAKE_NACK;
            frame_stage_process(parser);
            return true;
        }
    }
    /* 帧接收超时 → NAK */
    if (parser->frame_info.frame_is_start == true) {
        parser->frame_info.frame_is_start = false;
        parser->error = YMODEM_ERROR_TIME_OUT;
        frame_stage_process(parser);
        return true;
    }
    return false;
}

bool ymodem_receiver_start(ymodem_receiver_parser_t* parser)
{
    if (!parser) {
        return false;
    }
    /* 完全复位所有状态（模拟 create 时的初始化） */
    memset(&parser->process, 0, sizeof(parser->process));
    memset(&parser->file_info, 0, sizeof(parser->file_info));
    memset(&parser->frame_info, 0, sizeof(parser->frame_info));
    memset(&parser->user_evt, 0, sizeof(parser->user_evt));    
    
    parser->stat = YMODEM_RECV_WAIT_HEAD;
    parser->stage = YMODEM_STAGE_ESTABLISHING;
    parser->process.last_time_ms = system_get_time_ms();
        
    /* 设置握手错误，让 frame_stage_process 处理重发逻辑 */
    parser->error = YMODEM_ERROR_HANDSHAKE_NACK;
    //frame_stage_process(parser);
   
    return true;
}
