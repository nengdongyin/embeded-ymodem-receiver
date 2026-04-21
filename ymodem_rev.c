/**
 * @file ymodem_rev.c
 * @brief Ymodem协议解析器实现文件
 *
 * 包含了Ymodem协议解析器的具体实现逻辑。
 */

#include "ymodem_rev.h"

 /** @brief 帧头长度（字节） */
#define YMODEM_HEAD_LEN_BYTE									(3)
/** @brief CRC长度（字节） */
#define YMODEM_CRC_LEN_BYTE										(2)
/** @brief SOH数据长度（字节） */
#define YMODEM_SOH_DATA_LEN_BYTE								128
/** @brief STX数据长度（字节） */
#define YMODEM_STX_DATA_LEN_BYTE								1024
/** @brief SOH帧长度（字节） */
#define YMODEM_SOH_FRAME_LEN_BYTE   (YMODEM_HEAD_LEN_BYTE+YMODEM_SOH_DATA_LEN_BYTE+YMODEM_CRC_LEN_BYTE)
/** @brief STX帧长度（字节） */
#define YMODEM_STX_FRAME_LEN_BYTE   (YMODEM_HEAD_LEN_BYTE+YMODEM_STX_DATA_LEN_BYTE+YMODEM_CRC_LEN_BYTE)
/** @brief 序号字节索引 */
#define YMODEM_SEQ_BYTE_INDEX									(1)
/** @brief 序号反码字节索引 */
#define YMODEM_NOR_SEQ_BYTE_INDEX								(2)
/** @brief 数据字节起始索引 */
#define YMODEM_DATA_BYTE_INDEX									(3)

/**
 * @brief CRC16计算表 (XMODEM标准)
 */
static const uint16_t ccitt_table[256] =
{
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
    0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF,
    0x1231, 0x0210, 0x3273, 0x2252, 0x52B5, 0x4294, 0x72F7, 0x62D6,
    0x9339, 0x8318, 0xB37B, 0xA35A, 0xD3BD, 0xC39C, 0xF3FF, 0xE3DE,
    0x2462, 0x3443, 0x0420, 0x1401, 0x64E6, 0x74C7, 0x44A4, 0x5485,
    0xA56A, 0xB54B, 0x8528, 0x9509, 0xE5EE, 0xF5CF, 0xC5AC, 0xD58D,
    0x3653, 0x2672, 0x1611, 0x0630, 0x76D7, 0x66F6, 0x5695, 0x46B4,
    0xB75B, 0xA77A, 0x9719, 0x8738, 0xF7DF, 0xE7FE, 0xD79D, 0xC7BC,
    0x48C4, 0x58E5, 0x6886, 0x78A7, 0x0840, 0x1861, 0x2802, 0x3823,
    0xC9CC, 0xD9ED, 0xE98E, 0xF9AF, 0x8948, 0x9969, 0xA90A, 0xB92B,
    0x5AF5, 0x4AD4, 0x7AB7, 0x6A96, 0x1A71, 0x0A50, 0x3A33, 0x2A12,
    0xDBFD, 0xCBDC, 0xFBBF, 0xEB9E, 0x9B79, 0x8B58, 0xBB3B, 0xAB1A,
    0x6CA6, 0x7C87, 0x4CE4, 0x5CC5, 0x2C22, 0x3C03, 0x0C60, 0x1C41,
    0xEDAE, 0xFD8F, 0xCDEC, 0xDDCD, 0xAD2A, 0xBD0B, 0x8D68, 0x9D49,
    0x7E97, 0x6EB6, 0x5ED5, 0x4EF4, 0x3E13, 0x2E32, 0x1E51, 0x0E70,
    0xFF9F, 0xEFBE, 0xDFDD, 0xCFFC, 0xBF1B, 0xAF3A, 0x9F59, 0x8F78,
    0x9188, 0x81A9, 0xB1CA, 0xA1EB, 0xD10C, 0xC12D, 0xF14E, 0xE16F,
    0x1080, 0x00A1, 0x30C2, 0x20E3, 0x5004, 0x4025, 0x7046, 0x6067,
    0x83B9, 0x9398, 0xA3FB, 0xB3DA, 0xC33D, 0xD31C, 0xE37F, 0xF35E,
    0x02B1, 0x1290, 0x22F3, 0x32D2, 0x4235, 0x5214, 0x6277, 0x7256,
    0xB5EA, 0xA5CB, 0x95A8, 0x8589, 0xF56E, 0xE54F, 0xD52C, 0xC50D,
    0x34E2, 0x24C3, 0x14A0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
    0xA7DB, 0xB7FA, 0x8799, 0x97B8, 0xE75F, 0xF77E, 0xC71D, 0xD73C,
    0x26D3, 0x36F2, 0x0691, 0x16B0, 0x6657, 0x7676, 0x4615, 0x5634,
    0xD94C, 0xC96D, 0xF90E, 0xE92F, 0x99C8, 0x89E9, 0xB98A, 0xA9AB,
    0x5844, 0x4865, 0x7806, 0x6827, 0x18C0, 0x08E1, 0x3882, 0x28A3,
    0xCB7D, 0xDB5C, 0xEB3F, 0xFB1E, 0x8BF9, 0x9BD8, 0xABBB, 0xBB9A,
    0x4A75, 0x5A54, 0x6A37, 0x7A16, 0x0AF1, 0x1AD0, 0x2AB3, 0x3A92,
    0xFD2E, 0xED0F, 0xDD6C, 0xCD4D, 0xBDAA, 0xAD8B, 0x9DE8, 0x8DC9,
    0x7C26, 0x6C07, 0x5C64, 0x4C45, 0x3CA2, 0x2C83, 0x1CE0, 0x0CC1,
    0xEF1F, 0xFF3E, 0xCF5D, 0xDF7C, 0xAF9B, 0xBFBA, 0x8FD9, 0x9FF8,
    0x6E17, 0x7E36, 0x4E55, 0x5E74, 0x2E93, 0x3EB2, 0x0ED1, 0x1EF0
};
/**
 * @brief 使用查表法计算CRC16
 *
 * @param data 数据指针
 * @param size 数据大小
 * @return 计算得到的CRC16值
 */
static uint16_t calculate_crc16(const uint8_t* data, int size) {
    uint16_t crc = 0;
    while (size-- > 0)
        crc = (crc << 8) ^ ccitt_table[((crc >> 8) ^ *data++) & 0xff];
    return crc;
}

/**
 * @brief 复位Ymodem协议解析器
 *
 * 根据当前错误类型和阶段，执行不同程度的复位操作。
 *
 * @param parser 指向协议解析器句柄的指针
 * @return true 成功，false 失败
 */
static bool ymodem_protocol_reset(ymodem_protocol_parser_t* parser)
{
    if (!parser) {
        return false;
    }
    // 对于不同的帧类型和帧错误使用不同程度的复位

    //如果是正常帧
    if (parser->error == YMODEM_ERROR_NONE) {
        //当前帧错误计数清0,握手状态置为有效，握手计数清0
        parser->frame_info.current_frame_error_count = 0;
        parser->process.is_handshake_active = true;
        parser->process.handshake_count = 0;
        //取消传输类型，还要复位文件信息
        switch (parser->stage)
        {
        case YMODEM_STAGE_FINISHED:
        case YMODEM_STAGE_ABORTED: {
            memset(&parser->file_info, 0, sizeof(parser->file_info));
            break;
        }
        default:
            break;
        }
    }
    //如果是错误帧
    else {
        switch (parser->error)
        {
        case YMODEM_ERROR_TIME_OUT:
        case YMODEM_ERROR_CRC:
        case YMODEM_ERROR_SEQ: {
            break;
        }
        //重传上限，还要复位文件信息和连接处理状态
        case YMODEM_ERROR_RETRANSMISSION_COUNT_MAX: {
            memset(&parser->process, 0, sizeof(parser->process));
            memset(&parser->file_info, 0, sizeof(parser->file_info));
            // 强制复位握手标志，以便 Poll 重新发 C
            parser->process.is_handshake_active = false;
            parser->process.handshake_count = 0;
        }
        default:
            break;
        }

    }
    //通用复位逻辑，current_frame_error_count不清0
    uint32_t saved_error_count = parser->frame_info.current_frame_error_count;
    memset(&parser->frame_info, 0, sizeof(parser->frame_info));
    parser->frame_info.current_frame_error_count = saved_error_count;
    parser->error = YMODEM_ERROR_NONE;
    if (parser->stage != YMODEM_STAGE_FINISHING) {
        parser->stat = YMODEM_WAIT_HEAD;
    }
    return true;
}

/**
 * @brief 解析Ymodem文件信息帧
 *
 * 从接收缓冲区中解析出文件名和文件大小。
 *
 * @param parser 指向协议解析器句柄的指针
 * @return true 解析成功，false 解析失败
 */
static bool ymodem_protocol_file_info_parser(ymodem_protocol_parser_t* parser)
{
    if (!parser) {
        return 0;
    }
    uint8_t* data_ptr = &parser->buffer.rx_buffer[YMODEM_DATA_BYTE_INDEX];

    //解析文件名
    uint32_t i = 0;
    while (i < (sizeof(parser->file_info.file_name) - 1) && data_ptr[i] != '\0') {
        parser->file_info.file_name[i] = data_ptr[i];
        i++;
    }
    parser->file_info.file_name[i] = '\0';

    //跳过所有非数字字符（包括空格、\0 等）
    while (i < YMODEM_SOH_DATA_LEN_BYTE && (data_ptr[i] < '0' || data_ptr[i] > '9')) {
        i++;
    }
    data_ptr[YMODEM_SOH_DATA_LEN_BYTE - 1] = '\0';
    //解析文件大小
    if (i < YMODEM_SOH_DATA_LEN_BYTE) {
        parser->file_info.file_total_size = strtoul((char*)&data_ptr[i], NULL, 10);
        return true;
    }
    return false;
}

/**
 * @brief 为错误帧编码响应
 *
 * 根据错误类型，设置发送缓冲区的内容。
 *
 * @param parser 指向协议解析器句柄的指针
 * @return true 编码成功，false 编码失败
 */
static bool  frame_error_response_encode(ymodem_protocol_parser_t* parser)
{
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
    case YMODEM_ERROR_RETRANSMISSION_COUNT_MAX: {
        parser->buffer.tx_buffer_ack_len = 2;
        parser->buffer.tx_buffer[0] = YMODEM_CAN;
        parser->buffer.tx_buffer[1] = YMODEM_CAN;
        break;
    }
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
 * @brief 处理错误帧
 *
 * 根据错误类型，更新错误计数和阶段。
 *
 * @param parser 指向协议解析器句柄的指针
 * @return true 处理成功，false 处理失败
 */
static bool frame_error_process(ymodem_protocol_parser_t* parser)
{
    if (!parser) {
        return false;
    }
    switch (parser->error) {
    case YMODEM_ERROR_TIME_OUT:
    case YMODEM_ERROR_CRC:
    case YMODEM_ERROR_SEQ: {
        parser->frame_info.current_frame_error_count++;
        //错误太多，错误码升级为达到重传上限，强制取消
        if (parser->frame_info.current_frame_error_count > YMODEM_RETRANSMISSION_MAX_COUNT) {
            parser->error = YMODEM_ERROR_RETRANSMISSION_COUNT_MAX;
            parser->stage = YMODEM_STAGE_ABORTED;
        }
        return true;
    }
    case YMODEM_ERROR_HANDSHAKE_NACK: {
        parser->process.handshake_count++;
        //错误太多，错误码升级为达到重传上限，强制取消
        if (parser->process.handshake_count > YMODEM_RETRANSMISSION_MAX_COUNT) {
            parser->error = YMODEM_ERROR_RETRANSMISSION_COUNT_MAX;
            parser->stage = YMODEM_STAGE_ABORTED;
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
bool protocol_parser_create(ymodem_protocol_parser_t* parser, uint8_t* rx_buffer, uint32_t rx_buffer_size)
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
    memset(parser, 0, sizeof(ymodem_protocol_parser_t));
    parser->buffer.rx_buffer = rx_buffer;
    parser->buffer.rx_buffer_len = rx_buffer_size;
    parser->buffer.tx_buffer_len = sizeof(parser->buffer.tx_buffer);
    parser->stat = YMODEM_WAIT_HEAD;
    parser->process.last_time_ms = system_get_time_ms();
    parser->stage = YMODEM_STAGE_IDLE;
    return true;
}

/**
 * @brief 设置事件通知回调
 * @param parser   解析器实例
 * @param callback 回调函数
 * @param user_ctx 用户上下文
 */
bool ymodem_set_event_callback(ymodem_protocol_parser_t* parser, ymodem_event_callback_t callback, void* user_ctx)
{
    if (!parser) {
        return false;
    }
    parser->callbacks.event_callback = callback;
    parser->callbacks.event_user_ctx = user_ctx;
    return true;
}

/**
 * @brief 设置发送响应回调函数
 *
 * @param parser 指向协议解析器句柄的指针
 * @param send_response_cb 发送响应回调函数
 * @param user_ctx 用户上下文指针
 * @return true 成功，false 失败
 */
bool protocol_parser_set_send_response_callback(ymodem_protocol_parser_t* parser, send_response_t send_response_cb, void* user_ctx)
{
    if (!parser) {
        return false;
    }
    parser->callbacks.send_response = send_response_cb;
    parser->callbacks.send_response_user_ctx = user_ctx;
    return true;
}
/**
 * @brief
 *
 * 构造发送缓冲区NAK应答
 *
 * @param parser 指向协议解析器句柄的指针
 */
static bool frame_nak_without_data(ymodem_protocol_parser_t* parser)
{
    if (!parser) {
        return false;
    }
    //编码应答
    parser->buffer.tx_buffer_ack_len = 1;
    parser->buffer.tx_buffer[0] = YMODEM_ACK;
    return true;
}
/**
 * @brief
 *
 * 构造发送缓冲区ACK应答
 *
 * @param parser 指向协议解析器句柄的指针
 */
static bool frame_ack_without_data(ymodem_protocol_parser_t* parser)
{
    if (!parser) {
        return false;
    }
    //编码应答
    parser->buffer.tx_buffer_ack_len = 1;
    parser->buffer.tx_buffer[0] = YMODEM_ACK;
    return true;
}
/**
 * @brief
 *
 * 构造发送缓冲区ACK和C应答
 *
 * @param parser 指向协议解析器句柄的指针
 */
static bool frame_ack_c_without_data(ymodem_protocol_parser_t* parser)
{
    if (!parser) {
        return false;
    }
    //编码应答
    parser->buffer.tx_buffer_ack_len = 2;
    parser->buffer.tx_buffer[0] = YMODEM_ACK;
    parser->buffer.tx_buffer[1] = YMODEM_C;
    return true;
}
/**
 * @brief
 *
 * 构造用户事件
 *
 * @param parser 指向协议解析器句柄的指针
 */
static bool frame_user_event_set(ymodem_protocol_parser_t* parser, ymodem_event_type_t evt_type)
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


/**
 * @brief
 *
 * 构造发送缓冲区ACK和C应答，并调用用户frame就绪回调
 *
 * @param parser 指向协议解析器句柄的指针
 */
static bool frame_ack_c_width_data(ymodem_protocol_parser_t* parser, ymodem_event_type_t evt_type)
{
    if (!parser) {
        return false;
    }
    frame_ack_c_without_data(parser);
    frame_user_event_set(parser, evt_type);
    //通知用户收到文件信息,取走数据
    if (parser->callbacks.event_callback) {
        parser->callbacks.event_callback(parser, &parser->user_evt, parser->callbacks.event_user_ctx);
    }
    return true;
}

/**
 * @brief
 *
 * 构造发送缓冲区ACK应答，并调用用户frame就绪回调
 *
 * @param parser 指向协议解析器句柄的指针
 */
static bool frame_ack_width_data(ymodem_protocol_parser_t* parser, ymodem_event_type_t evt_type)
{
    if (!parser) {
        return false;
    }
    frame_ack_without_data(parser);
    frame_user_event_set(parser, evt_type);
    //通知用户收到文件信息,取走数据
    if (parser->callbacks.event_callback) {
        parser->callbacks.event_callback(parser, &parser->user_evt, parser->callbacks.event_user_ctx);
    }
    return true;
}
/**
 * @brief 处理帧和阶段转换
 *
 * 根据当前错误和阶段，执行相应的处理逻辑。
 *
 * @param parser 指向协议解析器句柄的指针
 */
static void frame_stage_process(ymodem_protocol_parser_t* parser)
{
    //对错误处理和编码
    if (parser->error != YMODEM_ERROR_NONE) {
        frame_error_process(parser);
        frame_error_response_encode(parser);
    }
    else {
        //只要收到取消，无论什么状态，直接取消
        if (parser->frame_info.frame_type == YMODEM_FRAME_TYPE_CAN) {
            parser->stage = YMODEM_STAGE_ABORTED;
            //通知用户出错了
            frame_ack_width_data(parser, YMODEM_EVENT_ERROR);
        }
        else {
            switch (parser->stage)
            {
            case YMODEM_STAGE_IDLE: {
                break;
            }
            case YMODEM_STAGE_ESTABLISHING: {
                if ((parser->frame_info.frame_type == YMODEM_FRAME_TYPE_SOH) && (parser->frame_info.current_frame_index == 0)) {
                    if (ymodem_protocol_file_info_parser(parser) == true) {
                        parser->stage = YMODEM_STAGE_ESTABLISHED;
                        parser->file_info.file_rev_frame_number++;
                        //通知用户文件信息包完成,取走数据
                        frame_ack_c_width_data(parser, YMODEM_EVENT_FILE_INFO);
                    }
                }
                break;
            }
            case YMODEM_STAGE_ESTABLISHED: {
                if (((parser->frame_info.frame_type == YMODEM_FRAME_TYPE_STX || parser->frame_info.frame_type == YMODEM_FRAME_TYPE_SOH))
                    && (parser->frame_info.current_frame_index == 1)) {
                    parser->file_info.file_rev_frame_number++;
                    parser->file_info.file_rev_size += parser->frame_info.current_frame_data_len;
                    parser->stage = YMODEM_STAGE_TRANSFERRING;
                    //通知用户数据包完成,取走数据
                    frame_ack_width_data(parser, YMODEM_EVENT_DATA_PACKET);
                }
                else {
                    ;
                }
                break;
            }
            case YMODEM_STAGE_TRANSFERRING: {
                if ((parser->frame_info.frame_type == YMODEM_FRAME_TYPE_STX) || (parser->frame_info.frame_type == YMODEM_FRAME_TYPE_SOH)) {
                    parser->file_info.file_rev_frame_number++;
                    parser->file_info.file_rev_size += parser->frame_info.current_frame_data_len;
                    //通知用户数据包完成,取走数据
                    frame_ack_width_data(parser, YMODEM_EVENT_DATA_PACKET);
                }
                else if (parser->frame_info.frame_type == YMODEM_FRAME_TYPE_EOT) {
                    parser->stage = YMODEM_STAGE_FINISHING;
                    //编码应答
                    frame_ack_without_data(parser);
                }
                else {
                    ;
                }
                break;
            }
            case YMODEM_STAGE_FINISHING: {
                if (parser->frame_info.frame_type == YMODEM_FRAME_TYPE_EOT) {
                    parser->stage = YMODEM_STAGE_FINISHED;
                    //通知用户数据包完成,取走数据
                    frame_ack_width_data(parser, YMODEM_EVENT_TRANSFER_COMPLETE);
                }
                else {
                    ;
                }
                break;
            }
            case YMODEM_STAGE_FINISHED: {
                if ((parser->frame_info.frame_type == YMODEM_FRAME_TYPE_SOH) && (parser->frame_info.current_frame_index == 0)) {
                    if (ymodem_protocol_file_info_parser(parser) == true) {
                        parser->stage = YMODEM_STAGE_ESTABLISHED;
                        //通知用户会话结束
                        frame_ack_c_width_data(parser, YMODEM_EVENT_SESSION_FINISHED);
                    }
                    else {
                        parser->stage = YMODEM_STAGE_ABORTED;
                        frame_ack_without_data(parser);
                    }
                }
                else {
                    frame_nak_without_data(parser);
                }
                break;
            }
            case YMODEM_STAGE_ABORTED: {
                // 回到空闲状态保持静默,必须重新开始start
                parser->stage = YMODEM_STAGE_IDLE;
                break;
            }
            default: {
                frame_nak_without_data(parser);
                break;
            }
            }
        }
    }
    //通知用户send回调,取走数据,通过通信接口发送
    if (parser->callbacks.send_response) {
        parser->callbacks.send_response(parser, parser->callbacks.send_response_user_ctx);
    }
    //复位解析器状态，等待下一帧
    ymodem_protocol_reset(parser);
}

/**
 * @brief Ymodem协议解析主函数
 *
 * 非阻塞式解析输入的数据流。
 *
 * @param parser 指向协议解析器句柄的指针
 * @param data 输入数据指针
 * @param len 输入数据长度
 */
void  ymodem_protocol_parser(ymodem_protocol_parser_t* parser, uint8_t* data, uint32_t len)
{
    if ((!parser) || (!data) || (!len)) {
        return;
    }
    parser->error = YMODEM_ERROR_NEED_MORE_DATA;
    for (int i = 0; i < len; i++) {
        uint8_t byte = data[i];
        if (parser->frame_info.current_frame_rev_len >= parser->buffer.rx_buffer_len) {
            parser->error = YMODEM_ERROR_CRC;//给个CRC错误，清除状态
            break;
        }
        switch (parser->stat)
        {
        case YMODEM_WAIT_CAN_2: {
            if (byte == YMODEM_CAN) {
                parser->frame_info.frame_type = YMODEM_FRAME_TYPE_CAN;
                parser->error = YMODEM_ERROR_NONE;
            }
            else {
                parser->stat = YMODEM_WAIT_HEAD;
                i--;
                break;
            }
            break;
        }
        case YMODEM_WAIT_HEAD: {
            if (byte == YMODEM_SOH) {
                parser->frame_info.frame_type = YMODEM_FRAME_TYPE_SOH;
                parser->frame_info.current_frame_total_len = YMODEM_SOH_FRAME_LEN_BYTE;
                parser->frame_info.current_frame_data_len = YMODEM_SOH_DATA_LEN_BYTE;
            }
            else if (byte == YMODEM_STX) {
                parser->frame_info.frame_type = YMODEM_FRAME_TYPE_STX;
                parser->frame_info.current_frame_total_len = YMODEM_STX_FRAME_LEN_BYTE;
                parser->frame_info.current_frame_data_len = YMODEM_STX_DATA_LEN_BYTE;
            }
            else if (byte == YMODEM_EOT) {
                parser->error = YMODEM_ERROR_NONE;
                parser->frame_info.frame_type = YMODEM_FRAME_TYPE_EOT;
                parser->stat = YMODEM_WAIT_HEAD;
                break;
            }
            else if (byte == YMODEM_CAN) {
                parser->frame_info.frame_type = YMODEM_FRAME_TYPE_CAN;
                parser->stat = YMODEM_WAIT_CAN_2;
                parser->frame_info.frame_is_start = true;//开启超时
                parser->process.last_time_ms = system_get_time_ms();
            }
            else {
                break;
            }
            parser->buffer.rx_buffer[parser->frame_info.current_frame_rev_len++] = byte;
            parser->stat = YMODEM_WAIT_SEQ;
            break;
        }
        case YMODEM_WAIT_SEQ: {
            parser->buffer.rx_buffer[parser->frame_info.current_frame_rev_len++] = byte;
            if (parser->frame_info.current_frame_rev_len == YMODEM_HEAD_LEN_BYTE) {
                uint8_t seq = parser->buffer.rx_buffer[YMODEM_SEQ_BYTE_INDEX];
                uint8_t seq_n = parser->buffer.rx_buffer[YMODEM_NOR_SEQ_BYTE_INDEX];
                if ((uint8_t)(seq + seq_n) == 0xFF) {
                    uint8_t expected_seq = parser->file_info.file_rev_frame_number & 0xFF;
                    uint8_t prev_seq = (expected_seq == 0) ? 0xFF : expected_seq - 1;
                    if (parser->buffer.rx_buffer[YMODEM_SEQ_BYTE_INDEX] == expected_seq) {
                        parser->stat = YMODEM_WAIT_DATA;

                    }
                    else if (parser->buffer.rx_buffer[YMODEM_SEQ_BYTE_INDEX] == prev_seq) {
                        parser->frame_info.current_frame_is_resend = true;
                        parser->stat = YMODEM_WAIT_DATA;
                    }
                    else {
                        parser->error = YMODEM_ERROR_SEQ;
                        break;
                    }
                    parser->frame_info.frame_is_start = true;
                    parser->frame_info.current_frame_index = parser->buffer.rx_buffer[YMODEM_SEQ_BYTE_INDEX];
                    parser->process.last_time_ms = system_get_time_ms();
                }
                else {
                    parser->error = YMODEM_ERROR_SEQ;
                    break;
                }
            }
            break;
        }
        case YMODEM_WAIT_DATA: {
            parser->buffer.rx_buffer[parser->frame_info.current_frame_rev_len++] = byte;
            if (parser->frame_info.current_frame_rev_len == parser->frame_info.current_frame_total_len - YMODEM_CRC_LEN_BYTE) {
                parser->stat = YMODEM_WAIT_CRC;
            }
            break;
        }
        case YMODEM_WAIT_CRC: {
            parser->buffer.rx_buffer[parser->frame_info.current_frame_rev_len++] = byte;
            if (parser->frame_info.current_frame_rev_len == parser->frame_info.current_frame_total_len) {
                uint16_t crc0_index = parser->frame_info.current_frame_total_len - 2;
                uint16_t crc1_index = parser->frame_info.current_frame_total_len - 1;
                uint16_t received_crc = (parser->buffer.rx_buffer[crc0_index] << 8) | parser->buffer.rx_buffer[crc1_index];
                uint16_t calculated_crc = calculate_crc16(&parser->buffer.rx_buffer[YMODEM_DATA_BYTE_INDEX], parser->frame_info.current_frame_data_len);
                if (received_crc != calculated_crc) {
                    parser->error = YMODEM_ERROR_CRC;
                    break;
                }
                else {
                    if (parser->frame_info.current_frame_is_resend == true) {
                        parser->error = YMODEM_ERROR_SENDER_NO_REV_ACK;
                    }
                    else {
                        parser->error = YMODEM_ERROR_NONE;
                    }
                    parser->process.is_handshake_active = true;
                    parser->frame_info.frame_is_end = true;
                }
            }
            break;
        }
        default:
            break;
        }
    }
    if (parser->error != YMODEM_ERROR_NEED_MORE_DATA) {
        frame_stage_process(parser);
    }
}

/**
 * @brief Ymodem协议超时处理函数
 *
 * 需要定期调用此函数来处理超时。
 *
 * @param parser 指向协议解析器句柄的指针
 */
void  ymodem_protocol_process_poll(ymodem_protocol_parser_t* parser)
{
    if (!parser) {
        return;
    }
    if (parser->stage == YMODEM_STAGE_IDLE) {
        return;
    }
    uint32_t now = system_get_time_ms();
    if (now - parser->process.last_time_ms < YMODEM_TIMEOUT_MS) {
        return;
    }
    parser->process.last_time_ms = now;
    if (parser->stage == YMODEM_STAGE_ESTABLISHING) {
        if (parser->process.is_handshake_active == false) {
            parser->error = YMODEM_ERROR_HANDSHAKE_NACK;
            frame_stage_process(parser);
        }
    }
    else if (parser->frame_info.frame_is_start == true) {
        parser->frame_info.frame_is_start = false;
        parser->error = YMODEM_ERROR_TIME_OUT;
        frame_stage_process(parser);
    }
    else {
        ;
    }
}
/**
 * @brief Ymodem协议解析器启动函数
 *
 * 需要使用该函数启动ymodem解析器发送起始C。
 *
 * @param parser 指向协议解析器句柄的指针
 */
void ymodem_protocol_start(ymodem_protocol_parser_t* parser)
{
    if (!parser) {
        return;
    }
    parser->stage = YMODEM_STAGE_ESTABLISHING;
    parser->error = YMODEM_ERROR_HANDSHAKE_NACK;
    frame_stage_process(parser);
}
/**
 * @brief 获取系统时间（毫秒）
 *
 * 此函数需要用户实现。
 *
 * @return 当前系统时间（毫秒）
 */
__attribute__((weak)) uint32_t system_get_time_ms(void)
{
    return 0;
}
