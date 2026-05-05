/**
 * @file ymodem_sender.c
 * @brief Ymodem 协议发送器实现
 *
 * 实现 Ymodem 协议发送端的状态机和帧构建逻辑。
 *
 * @par 协议状态流转（概览）
 * @code
 *   create() → IDLE
 *     → start() → ESTABLISHING
 *       → 收 'C'  → FILE_INFO 回调 → ESTABLISHED
 *         → (file_total_size==0?) → EOT → FINISHING
 *         → DATA_PACKET 回调 → TRANSFERRING
 *           → 每收 ACK → DATA_PACKET 回调 ...
 *           → file_send_size >= file_total_size → EOT → FINISHING
 *             → EOT → FINISHED
 *               → TRANSFER_COMPLETE 回调, file_index++
 *               → ESTABLISHING (等待下一文件或空文件名结束)
 * @endcode
 */

#include "ymodem_sender.h"
#include <string.h>
#include <stdio.h>

/* ================================================================
 *  内部辅助函数
 * ================================================================ */

/**
 * @brief 发送数据包并刷新超时基准时间
 *
 * 统一封装所有 send_packet 回调调用点。
 * 每次发包后更新 last_time_ms，确保 poll 超时从当前发包时刻起算。
 *
 * @param send 发送器实例
 */
static void sender_send_packet(ymodem_sender_t* send)
{
    if (send->callbacks.send_packet != NULL) {
        send->callbacks.send_packet(send, &send->user_evt,
            send->callbacks.send_packet_user_ctx);
    }
    send->process.last_time_ms = system_get_time_ms();
}

/**
 * @brief 复位发送器内部状态
 *
 * 根据当前错误类型和协议阶段，执行不同程度的复位：
 * - 正常帧 (NONE): 清 frame_error_count, ABORTED 时额外清 file_info
 * - 超时/重发 (TIME_OUT/RESEND): 不清除（保持重传上下文）
 * - CAN 错误: 清 process 和 file_info
 *
 * @param send 发送器实例
 */
void ymodem_sender_reset(ymodem_sender_t* send)
{
    if (!send) {
        return;
    }

    /* ---- 阶段复位（按当前错误类型分级） ---- */
    if (send->error == YMODEM_ERROR_NONE) {
        send->frame_info.current_frame_error_count = 0;
        switch (send->stage)
        {
        case YMODEM_STAGE_ABORTED: {
            memset(&send->file_info, 0, sizeof(send->file_info));
            break;
        }
        default:
            break;
        }
    }
    else {
        switch (send->error)
        {
        case YMODEM_ERROR_TIME_OUT:
        case YMODEM_ERROR_RESEND: {
            break;
        }
        case YMODEM_ERROR_CAN: {
            memset(&send->process, 0, sizeof(send->process));
            memset(&send->file_info, 0, sizeof(send->file_info));
            break;
        }
        default:
            break;
        }
    }

    /* ---- 通用复位：清 frame_info，保留 error_count ---- */
    uint32_t saved_error_count = send->frame_info.current_frame_error_count;
    memset(&send->frame_info, 0, sizeof(send->frame_info));
    send->frame_info.current_frame_error_count = saved_error_count;
    send->error = YMODEM_ERROR_NONE;
    if (send->stage != YMODEM_STAGE_FINISHING) {
        send->stat = YMODEM_SENDER_WAIT_ACK;
    }
}

/* ================================================================
 *  帧构建
 * ================================================================ */

/**
 * @brief 在 tx_buffer 中组装数据帧
 *
 * 填充帧头类型、序号、反码、数据区、填充字节 (0x1A) 及 CRC16。
 * 调用后 tx_buffer_active_len 设为帧总长度。
 *
 * @param send 发送器实例
 */
static void build_data_packet(ymodem_sender_t* send) {
    if (!send) {
        return;
    }
    /* 帧头：类型 + 序号 + 反码 */
    send->buffer.tx_buffer[YMODEM_FRAME_TYPE_BYTE_INDEX] = 
        (send->frame_info.frame_type == YMODEM_FRAME_TYPE_STX) ? YMODEM_STX : YMODEM_SOH;
    send->buffer.tx_buffer[YMODEM_SEQ_BYTE_INDEX] = send->file_info.file_send_frame_number;
    send->buffer.tx_buffer[YMODEM_NOR_SEQ_BYTE_INDEX] = ~send->buffer.tx_buffer[YMODEM_SEQ_BYTE_INDEX];

    /* 数据有效长度：由用户回调填充，为 0 则使用全容量 */
    if (send->frame_info.current_frame_data_active_len == 0) {
        send->frame_info.current_frame_data_active_len = send->frame_info.current_frame_data_len;
    }
    /* 尾部填充 0x1A (SUB) */
    uint32_t padding_len = send->frame_info.current_frame_total_len
        - YMODEM_HEAD_LEN_BYTE - YMODEM_CRC_LEN_BYTE
        - send->frame_info.current_frame_data_active_len;
    if (padding_len > 0) {
        memset(&send->buffer.tx_buffer[YMODEM_DATA_BYTE_INDEX + send->frame_info.current_frame_data_active_len],
            0x1A, padding_len);
    }

    /* CRC16 校验（对全容量数据区） */
    uint16_t crc = ymodem_calculate_crc16(&send->buffer.tx_buffer[YMODEM_DATA_BYTE_INDEX],
        send->frame_info.current_frame_data_len);
    send->buffer.tx_buffer[send->frame_info.current_frame_total_len - 2] = (crc >> 8) & 0xFF;
    send->buffer.tx_buffer[send->frame_info.current_frame_total_len - 1] = crc & 0xFF;

    send->buffer.tx_buffer_active_len = send->frame_info.current_frame_total_len;
}

/**
 * @brief 将文件名和大小打包到 tx_buffer 数据区
 *
 * 格式：文件名字符串 + '\0' + 十进制大小字符串
 *
 * @param send 发送器实例
 * @return true 打包成功
 * @return false 参数非法或文件名为空
 */
static bool ymodem_sender_file_info_pack(ymodem_sender_t* send)
{
    if (!send) {
        return false;
    }
    if (send->file_info.file_name[0] == '\0') {
        return false;
    }

    size_t name_len = strlen(send->file_info.file_name);

    char size_str[16];
    snprintf(size_str, sizeof(size_str), "%lu",
        (unsigned long)send->file_info.file_total_size);
    size_t size_len = strlen(size_str);

    /* 文件名长度不能超过 SOH 数据区 - 1（'\0'） - 大小字符串长度 */
    size_t max_name = YMODEM_SOH_DATA_LEN_BYTE - 1 - size_len;
    if (name_len > max_name) {
        name_len = max_name;
    }
    if (name_len > 0) {
        memcpy(&send->buffer.tx_buffer[YMODEM_DATA_BYTE_INDEX], send->file_info.file_name, name_len);
    }
    send->buffer.tx_buffer[YMODEM_DATA_BYTE_INDEX + name_len] = 0;
    memcpy(&send->buffer.tx_buffer[YMODEM_DATA_BYTE_INDEX + name_len + 1], size_str, size_len);

    send->frame_info.current_frame_data_active_len = send->frame_info.current_frame_data_len;
    return true;
}

/**
 * @brief 构造用户事件并调用事件回调
 *
 * @param send     发送器实例
 * @param evt_type 事件类型
 * @return true 成功
 * @return false send 为 NULL
 */
static bool frame_user_event_callback(ymodem_sender_t* send, ymodem_sender_event_type_t evt_type)
{
    if (!send) {
        return false;
    }
    send->user_evt.type = evt_type;
    send->user_evt.data = &send->buffer.tx_buffer[YMODEM_DATA_BYTE_INDEX];
    send->user_evt.file_index = send->file_info.file_index;
    send->user_evt.file_name = send->file_info.file_name;
    send->user_evt.file_size = send->file_info.file_total_size;
    send->user_evt.data_seq  = send->file_info.file_send_frame_number > 1 ? send->file_info.file_send_frame_number-1 : 0; 
    

    if (send->callbacks.send_event_callback != NULL) {
        send->callbacks.send_event_callback(send, &send->user_evt, send->callbacks.send_event_user_ctx);
    }
    return true;
}

/**
 * @brief 构建带数据的帧（文件信息帧 或 数据帧）
 *
 * 根据事件类型选择 SOH（128B）或 STX（1K）帧格式，
 * 回调用户填充数据后组装完整帧。
 *
 * @param send     发送器实例
 * @param evt_type 事件类型（FILE_INFO 或 DATA_PACKET）
 * @return true 构建成功
 * @return false 构建失败（文件信息打包失败）
 */
static bool frame_build_with_data(ymodem_sender_t* send, ymodem_sender_event_type_t evt_type)
{
    if (!send) {
        return false;
    }
    switch (evt_type)
    {
    /* ---- 文件信息帧 (seq=0, SOH) ---- */
    case YMODEM_SENDER_EVENT_FILE_INFO: {
        frame_user_event_callback(send, evt_type);
        send->frame_info.frame_type = YMODEM_FRAME_TYPE_SOH;
        send->frame_info.current_frame_total_len = YMODEM_SOH_FRAME_LEN_BYTE;
        send->frame_info.current_frame_data_len = YMODEM_SOH_DATA_LEN_BYTE;
        memset(send->buffer.tx_buffer, 0, YMODEM_SOH_FRAME_LEN_BYTE);
        if (ymodem_sender_file_info_pack(send) == true) {
            build_data_packet(send);
            send->file_info.file_send_frame_number++;
        }
        else {
            return false;
        }
        break;
    }
    /* ---- 数据帧 (seq>=1, SOH 或 STX) ---- */
    case YMODEM_SENDER_EVENT_DATA_PACKET: {
        if (send->data_1k_enable) {
            send->frame_info.frame_type = YMODEM_FRAME_TYPE_STX;
            send->frame_info.current_frame_total_len = YMODEM_STX_FRAME_LEN_BYTE;
            send->frame_info.current_frame_data_len = YMODEM_STX_DATA_LEN_BYTE;
        }
        else {
            send->frame_info.frame_type = YMODEM_FRAME_TYPE_SOH;
            send->frame_info.current_frame_total_len = YMODEM_SOH_FRAME_LEN_BYTE;
            send->frame_info.current_frame_data_len = YMODEM_SOH_DATA_LEN_BYTE;
        }
        frame_user_event_callback(send, evt_type);
        send->frame_info.current_frame_data_active_len = send->user_evt.data_len;
        if (send->frame_info.current_frame_data_active_len == 0) {
            send->frame_info.current_frame_data_active_len = send->frame_info.current_frame_data_len;
        }
        build_data_packet(send);
        send->file_info.file_send_frame_number++;
        send->file_info.file_send_size += send->frame_info.current_frame_data_active_len;
        break;
    }
    default:
        break;
    }

    sender_send_packet(send);
    return true;
}

/**
 * @brief 处理错误帧
 *
 * 处理超时和重发错误：
 * - 递增 current_frame_error_count
 * - 超限则发送 CAN 并切换到 ABORTED 阶段
 * - 未超限则重发 tx_buffer 中的当前帧
 *
 * @param send 发送器实例
 * @return true 已处理
 * @return false 未知错误类型
 */
static bool frame_error_process(ymodem_sender_t* send)
{
    if (!send) {
        return false;
    }
    switch (send->error) {
    case YMODEM_ERROR_TIME_OUT:
    case YMODEM_ERROR_RESEND: {
        send->frame_info.current_frame_error_count++;
        /* 重传超限 → 取消传输 */
        if (send->frame_info.current_frame_error_count > YMODEM_RETRANSMISSION_MAX_COUNT) {
            send->buffer.tx_buffer[0] = YMODEM_CAN;
            send->buffer.tx_buffer[1] = YMODEM_CAN;
            send->buffer.tx_buffer_active_len = 2;
            sender_send_packet(send);
            frame_user_event_callback(send, YMODEM_SENDER_EVENT_ERROR);
            send->error = YMODEM_ERROR_CAN;
            send->stage = YMODEM_STAGE_ABORTED;
        }
        else {
            /* 未超限 → 重发当前帧 */
            if (send->buffer.tx_buffer_active_len > 0) {
                sender_send_packet(send);
            }
        }
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
 * @brief 协议阶段处理主状态机
 *
 * 根据当前 send->stage 和 send->error 执行对应的帧构建/发送/阶段切换。
 * 调用完毕后自动执行 ymodem_sender_reset() 清理帧级状态。
 *
 * @par 阶段流转图
 * @code
 *   IDLE ──start()──→ ESTABLISHING ──C──→ ESTABLISHED ──ACK+C──→ TRANSFERRING
 *                                            │       (file_size==0→EOT)      │
 *                                            └──→ FINISHING ←────────────────┘
 *                                                   │ NAK→EOT
 *                                                   └──→ FINISHED ──ACK──→ ESTABLISHING
 *                                                                            │
 *   CAN (任何阶段) ──→ ABORTED ──C──→ ESTABLISHING
 * @endcode
 *
 * @param send 发送器实例
 */
static void frame_stage_process(ymodem_sender_t* send)
{
    if (!send) {
        return;
    }

    /* ================================================================
     * 分支 1: 收到 CAN → 无条件取消
     * ================================================================ */
    if (send->error == YMODEM_ERROR_CAN) {
        send->buffer.tx_buffer[0] = YMODEM_CAN;
        send->buffer.tx_buffer[1] = YMODEM_CAN;
        send->buffer.tx_buffer_active_len = 2;
        sender_send_packet(send);
        send->stage = YMODEM_STAGE_ABORTED;
        frame_user_event_callback(send, YMODEM_SENDER_EVENT_ERROR);
    }
    /* ================================================================
     * 分支 2: 正常状态流转（按 stage 分发）
     * ================================================================ */
    else if (send->error == YMODEM_ERROR_NONE) {
        switch (send->stage)
        {
        /* ------------------------------------------------------------
         * IDLE: 空闲，不处理任何帧
         * ------------------------------------------------------------ */
        case YMODEM_STAGE_IDLE: {
            break;
        }

        /* ------------------------------------------------------------
         * ESTABLISHING: 收到接收方 'C' 后发送文件信息帧
         * 若 file_name 为空 → 发送空包结束会话
         * ------------------------------------------------------------ */
        case YMODEM_STAGE_ESTABLISHING: {
            send->file_info.file_send_frame_number = 0;
            send->file_info.file_send_size = 0;
            if (frame_build_with_data(send, YMODEM_SENDER_EVENT_FILE_INFO) == false) {
                /* 文件名留空 → 用户决定会话结束 */
                if (send->file_info.file_name[0] == '\0') {
                    memset(send->buffer.tx_buffer, 0, YMODEM_SOH_FRAME_LEN_BYTE);
                    send->buffer.tx_buffer[YMODEM_FRAME_TYPE_BYTE_INDEX] = YMODEM_SOH;
                    send->buffer.tx_buffer[YMODEM_SEQ_BYTE_INDEX] = 0;
                    send->buffer.tx_buffer[YMODEM_NOR_SEQ_BYTE_INDEX] = 0xFF;
                    uint16_t crc = ymodem_calculate_crc16(
                        &send->buffer.tx_buffer[YMODEM_DATA_BYTE_INDEX], YMODEM_SOH_DATA_LEN_BYTE);
                    send->buffer.tx_buffer[YMODEM_SOH_FRAME_LEN_BYTE - 2] = (crc >> 8) & 0xFF;
                    send->buffer.tx_buffer[YMODEM_SOH_FRAME_LEN_BYTE - 1] = crc & 0xFF;
                    send->buffer.tx_buffer_active_len = YMODEM_SOH_FRAME_LEN_BYTE;
                    sender_send_packet(send);
                    frame_user_event_callback(send, YMODEM_SENDER_EVENT_SESSION_FINISHED);
                    send->stage = YMODEM_STAGE_IDLE;
                } else {
                    send->error = YMODEM_ERROR_CAN;
                    send->stage = YMODEM_STAGE_ABORTED;
                }
                break;
            }
            send->stage = YMODEM_STAGE_ESTABLISHED;
            break;
        }

        /* ------------------------------------------------------------
         * ESTABLISHED: 发送首个数据帧。若文件大小为 0 则直接发 EOT
         * ------------------------------------------------------------ */
        case YMODEM_STAGE_ESTABLISHED: {
            /* 空文件：直接进入 FINISHING */
            if (send->file_info.file_total_size == 0) {
                send->buffer.tx_buffer[0] = YMODEM_EOT;
                send->buffer.tx_buffer_active_len = 1;
                sender_send_packet(send);
                send->stat = YMODEM_SENDER_WAIT_ACK;
                send->stage = YMODEM_STAGE_FINISHING;
                break;
            }
            /* 正常：发送首个数据帧后切换为 TRANSFERRING */
            if (frame_build_with_data(send, YMODEM_SENDER_EVENT_DATA_PACKET) == false) {
                send->error = YMODEM_ERROR_CAN;
                send->stage = YMODEM_STAGE_ABORTED;
                break;
            }
            send->stage = YMODEM_STAGE_TRANSFERRING;
            break;
        }

        /* ------------------------------------------------------------
         * TRANSFERRING: 持续发送数据帧，直到文件发完 → EOT
         * ------------------------------------------------------------ */
        case YMODEM_STAGE_TRANSFERRING: {
            /* 文件发送完毕 → 进入 FINISHING */
            if (send->file_info.file_send_size >= send->file_info.file_total_size) {
                send->buffer.tx_buffer[0] = YMODEM_EOT;
                send->buffer.tx_buffer_active_len = 1;
                sender_send_packet(send);
                send->stat = YMODEM_SENDER_WAIT_ACK;
                send->stage = YMODEM_STAGE_FINISHING;
            }
            /* 继续发送数据帧 */
            else if (frame_build_with_data(send, YMODEM_SENDER_EVENT_DATA_PACKET) == false) {
                send->error = YMODEM_ERROR_CAN;
                send->stage = YMODEM_STAGE_ABORTED;
            }
            break;
        }

        /* ------------------------------------------------------------
         * FINISHING: 发送第二个 EOT
         * ------------------------------------------------------------ */
        case YMODEM_STAGE_FINISHING: {
            send->buffer.tx_buffer[0] = YMODEM_EOT;
            send->buffer.tx_buffer_active_len = 1;
            sender_send_packet(send);
            send->stage = YMODEM_STAGE_FINISHED;
            break;
        }

        /* ------------------------------------------------------------
         * FINISHED: 通知用户文件传输完成，递增 file_index
         * ------------------------------------------------------------ */
        case YMODEM_STAGE_FINISHED: {
            frame_user_event_callback(send, YMODEM_SENDER_EVENT_TRANSFER_COMPLETE);
            send->file_info.file_index++;
            send->stage = YMODEM_STAGE_ESTABLISHING;
            break;
        }

        /* ------------------------------------------------------------
         * ABORTED: 取消后的重启 — 收到 'C' 后询问用户下一文件
         * ------------------------------------------------------------ */
        case YMODEM_STAGE_ABORTED: {
            if (frame_build_with_data(send, YMODEM_SENDER_EVENT_FILE_INFO) == false) {
                send->stage = YMODEM_STAGE_IDLE;
                frame_user_event_callback(send, YMODEM_SENDER_EVENT_ERROR);
                break;
            }
            if (send->file_info.file_total_size > 0) {
                send->stage = YMODEM_STAGE_TRANSFERRING;
            }
            else {
                send->stage = YMODEM_STAGE_FINISHING;
            }
            break;
        }

        default: {
            break;
        }
        }
    }
    /* ================================================================
     * 分支 3: 非 CAN 错误 → 走错误处理路径
     * ================================================================ */
    else {
        frame_error_process(send);
    }
    ymodem_sender_reset(send);
}

/* ================================================================
 *  公开 API
 * ================================================================ */

bool ymodem_sender_create(ymodem_sender_t* send, uint8_t* tx_buffer, uint32_t tx_buffer_size)
{
    if (!send) {
        return false;
    }
    if (!tx_buffer) {
        return false;
    }
    if (tx_buffer_size < YMODEM_STX_FRAME_LEN_BYTE) {
        return false;
    }
    memset(send, 0, sizeof(ymodem_sender_t));
    send->buffer.tx_buffer = tx_buffer;
    send->stat = YMODEM_SENDER_WAIT_C;
    send->process.last_time_ms = system_get_time_ms();
    send->stage = YMODEM_STAGE_IDLE;
    return true;
}

bool ymodem_sender_set_event_callback(ymodem_sender_t* send, ymodem_sender_event_callback_t callback, void* user_ctx)
{
    if (!send) {
        return false;
    }
    send->callbacks.send_event_callback = callback;
    send->callbacks.send_event_user_ctx = user_ctx;
    return true;
}

bool ymodem_sender_set_send_packet_callback(ymodem_sender_t* send,
    ymodem_sender_send_packet_t comm_cb, void* user_ctx)
{
    if (!send) {
        return false;
    }
    send->callbacks.send_packet = comm_cb;
    send->callbacks.send_packet_user_ctx = user_ctx;
    return true;
}

/**
 * @brief 发送器响应解析
 *
 * 逐字节扫描输入数据，按 stage/stat 两层状态机匹配响应：
 * - ESTABLISHING: 只响应 'C'
 * - ESTABLISHED:  解析 ACK+C 序列进入 TRANSFERRING；NAK 触发重发
 * - TRANSFERRING: ACK 继续发数据帧；NAK 重发当前帧
 * - FINISHING:    NAK 重发 EOT
 * - FINISHED:     ACK 进入下一文件；NAK 重发
 * - ABORTED:      'C' 尝试重启
 *
 * @return ymodem_error_e 本次调用结果：
 *         - YMODEM_ERROR_NONE    响应被成功处理
 *         - YMODEM_ERROR_GARBAGE   数据非 Ymodem 响应
 *         - YMODEM_ERROR_WAIT_MORE 未触发处理（如等待第二个 CAN）
 *         - 其他                  — 错误码
 */
ymodem_error_e ymodem_sender_parse(ymodem_sender_t* send, const uint8_t* data, uint32_t len)
{
    if ((!send) || (!data) || (!len)) {
        return YMODEM_ERROR_WAIT_MORE;
    }
    send->error = YMODEM_ERROR_NONE;

    bool acted = false;
    bool acknowledged = false;
    bool garbage = false;

    for (uint32_t i = 0; i < len; i++) {
        uint8_t byte = data[i];

        /* ---- CAN 两字节序列检测（与 stage 无关） ---- */
        if (byte == YMODEM_CAN) {
            acknowledged = true;
            if (send->stat == YMODEM_SENDER_WAIT_CAN_2) {
                send->error = YMODEM_ERROR_CAN;
                frame_stage_process(send);
                acted = true;
            }
            else {
                send->stat = YMODEM_SENDER_WAIT_CAN_2;
            }
        }
        /* ---- 非 CAN 字节：按 stage 分发 ---- */
        else {
            switch (send->stage)
            {
            /* ESTABLISHING: 只接受 'C' */
            case YMODEM_STAGE_ESTABLISHING: {
                if (byte == YMODEM_C) {
                    frame_stage_process(send);
                    acted = true;
                }
                else {
                    garbage = true;
                }
                break;
            }

            /* ESTABLISHED: ACK + C 组合 → TRANSFERRING */
            case YMODEM_STAGE_ESTABLISHED: {
                acknowledged = true;
                if (byte == YMODEM_NAK) {
                    send->error = YMODEM_ERROR_RESEND;
                    frame_stage_process(send);
                    acted = true;
                }
                else if (byte == YMODEM_ACK || byte == YMODEM_C) {
                    if (send->stat == YMODEM_SENDER_WAIT_ACK) {
                        if (byte == YMODEM_ACK) {
                            send->stat = YMODEM_SENDER_WAIT_C;
                        }
                    }
                    else if (send->stat == YMODEM_SENDER_WAIT_C) {
                        if (byte == YMODEM_C) {
                            frame_stage_process(send);
                            acted = true;
                        }
                        else {
                            send->stat = YMODEM_SENDER_WAIT_ACK;
                        }
                    }
                    else {
                        send->stat = YMODEM_SENDER_WAIT_ACK;
                    }
                }
                else {
                    garbage = true;
                }
                break;
            }

            /* ABORTED: 'C' 尝试重启 */
            case YMODEM_STAGE_ABORTED: {
                if (byte == YMODEM_C) {
                    frame_stage_process(send);
                    acted = true;
                }
                else {
                    garbage = true;
                }
                break;
            }

            /* TRANSFERRING: ACK → 下一帧; NAK → 重发 */
            case YMODEM_STAGE_TRANSFERRING: {
                if (byte == YMODEM_ACK) {
                    frame_stage_process(send);
                    acted = true;
                }
                else if (byte == YMODEM_NAK) {
                    send->error = YMODEM_ERROR_RESEND;
                    frame_stage_process(send);
                    acted = true;
                }
                else {
                    garbage = true;
                }
                break;
            }

            /* FINISHING: NAK → 重发 EOT; 其他也触发重发（任何字节都处理） */
            case YMODEM_STAGE_FINISHING: {
                if (byte == YMODEM_NAK) {
                    frame_stage_process(send);
                }
                else {
                    send->error = YMODEM_ERROR_RESEND;
                    frame_stage_process(send);
                }
                acted = true;
                break;
            }

            /* FINISHED: ACK → TRANSFER_COMPLETE; NAK → 重发 */
            case YMODEM_STAGE_FINISHED: {
                if (byte == YMODEM_ACK) {
                    frame_stage_process(send);
                    acted = true;
                }
                else if (byte == YMODEM_NAK) {
                    send->error = YMODEM_ERROR_RESEND;
                    frame_stage_process(send);
                    acted = true;
                }
                else {
                    garbage = true;
                }
                break;
            }

            /* IDLE / unknown stage: 所有字节均为垃圾 */
            default: {
                garbage = true;
                break;
            }
            }
        }
    }

    if (acted) {
        return send->error;
    }
    if (garbage) {
        return YMODEM_ERROR_GARBAGE;
    }
    if (acknowledged) {
        return YMODEM_ERROR_NONE;
    }
    return YMODEM_ERROR_WAIT_MORE;
}

bool ymodem_sender_poll(ymodem_sender_t* send)
{
    if (!send) {
        return false;
    }
    if (send->stage == YMODEM_STAGE_IDLE) {
        return false;
    }
    uint32_t now = system_get_time_ms();
    if (now - send->process.last_time_ms < YMODEM_TIMEOUT_MS) {
        return false;
    }
    send->process.last_time_ms = now;
    send->error = YMODEM_ERROR_TIME_OUT;
    frame_stage_process(send);
    return true;
}

bool ymodem_sender_start(ymodem_sender_t* send)
{
    if (!send) {
        return false;
    }
    send->stage = YMODEM_STAGE_ESTABLISHING;
    return true;
}

void ymodem_sender_enable_1k(ymodem_sender_t* send)
{
    if (!send) {
        return;
    }
    send->data_1k_enable = true;
}
