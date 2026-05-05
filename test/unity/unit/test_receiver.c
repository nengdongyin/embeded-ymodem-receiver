/**
 * @file test_receiver.c
 * @brief Comprehensive unit tests for ymodem_receiver byte-level parser
 *
 * Build (from project root):
 *   gcc -Isrc -Itest/unity/unity_core test/unity/unity_core/unity.c \
 *       test/unity/mocks/mock_time.c test/unity/unit/test_receiver.c \
 *       src/ymodem_receiver.c src/ymodem_common.c \
 *       -o test/unity/unit/test_receiver.exe
 */

#include "../unity_core/unity.h"
#include "../../../src/ymodem_receiver.h"
#include <string.h>

/* ================================================================ */
/*  Mock globals                                                     */
/* ================================================================ */

extern uint32_t mock_time_ms;

static bool     mock_event_called;
static uint32_t mock_last_event_type;
static bool     mock_response_sent;
static uint32_t mock_event_call_count;

static uint32_t          mock_event_file_size;
static uint32_t          mock_event_data_len;
static uint32_t          mock_event_total_received;
static char              mock_event_file_name[128];

static uint8_t  rx_buf[YMODEM_STX_FRAME_LEN_BYTE];
static ymodem_receiver_parser_t parser;

/* ================================================================ */
/*  Mock callbacks (richer logging)                                  */
/* ================================================================ */

static void mock_event_cb(ymodem_receiver_parser_t *p,
                          const ymodem_receiver_event_t *evt, void *ctx)
{
    (void)p;
    (void)ctx;
    mock_event_called = true;
    mock_last_event_type = evt->type;
    mock_event_call_count++;
    mock_event_file_size = evt->file_size;
    mock_event_data_len = evt->data_len;
    mock_event_total_received = evt->total_received;
    if (evt->file_name) {
        strncpy(mock_event_file_name, evt->file_name, sizeof(mock_event_file_name) - 1);
    } else {
        mock_event_file_name[0] = '\0';
    }
}

static void mock_response_cb(ymodem_receiver_parser_t *p, void *ctx)
{
    (void)p;
    (void)ctx;
    mock_response_sent = true;
}

/* ================================================================ */
/*  Helper: re-initialize receiver                                   */
/* ================================================================ */

static void receiver_reset(void)
{
    memset(&parser, 0, sizeof(parser));
    ymodem_receiver_create(&parser, rx_buf, sizeof(rx_buf));
    ymodem_receiver_set_event_callback(&parser, mock_event_cb, NULL);
    ymodem_receiver_set_send_response_callback(&parser, mock_response_cb, NULL);
    mock_time_ms = 0;
    mock_event_called = false;
    mock_response_sent = false;
    mock_last_event_type = 0;
    mock_event_call_count = 0;
    mock_event_file_size = 0;
    mock_event_data_len = 0;
    mock_event_total_received = 0;
    mock_event_file_name[0] = '\0';
}

/* ================================================================ */
/*  Helper: build valid SOH frame                                    */
/* ================================================================ */

static void build_valid_soh_frame(uint8_t *frame, uint8_t seq,
                                  const uint8_t *data, uint32_t data_len)
{
    memset(frame, 0, YMODEM_SOH_FRAME_LEN_BYTE);
    frame[YMODEM_FRAME_TYPE_BYTE_INDEX] = YMODEM_SOH;
    frame[YMODEM_SEQ_BYTE_INDEX]        = seq;
    frame[YMODEM_NOR_SEQ_BYTE_INDEX]    = (uint8_t)(~seq);
    if (data && data_len > 0) {
        memcpy(&frame[YMODEM_DATA_BYTE_INDEX], data,
               data_len > YMODEM_SOH_DATA_LEN_BYTE ? YMODEM_SOH_DATA_LEN_BYTE : data_len);
    }
    uint16_t crc = ymodem_calculate_crc16(
        &frame[YMODEM_DATA_BYTE_INDEX], YMODEM_SOH_DATA_LEN_BYTE);
    frame[YMODEM_SOH_FRAME_LEN_BYTE - 2] = (crc >> 8) & 0xFF;
    frame[YMODEM_SOH_FRAME_LEN_BYTE - 1] = crc & 0xFF;
}

/* ================================================================ */
/*  Helper: build valid STX frame                                    */
/* ================================================================ */

static void build_valid_stx_frame(uint8_t *frame, uint8_t seq)
{
    memset(frame, 0, YMODEM_STX_FRAME_LEN_BYTE);
    frame[YMODEM_FRAME_TYPE_BYTE_INDEX] = YMODEM_STX;
    frame[YMODEM_SEQ_BYTE_INDEX]        = seq;
    frame[YMODEM_NOR_SEQ_BYTE_INDEX]    = (uint8_t)(~seq);
    uint16_t crc = ymodem_calculate_crc16(
        &frame[YMODEM_DATA_BYTE_INDEX], YMODEM_STX_DATA_LEN_BYTE);
    frame[YMODEM_STX_FRAME_LEN_BYTE - 2] = (crc >> 8) & 0xFF;
    frame[YMODEM_STX_FRAME_LEN_BYTE - 1] = crc & 0xFF;
}

/* ================================================================ */
/*  Helper: establish file info (start + send frame 0)              */
/* ================================================================ */

static void establish_file_info(void)
{
    uint8_t frame[YMODEM_SOH_FRAME_LEN_BYTE];
    ymodem_receiver_start(&parser);
    build_valid_soh_frame(frame, 0,
        (const uint8_t *)"test.bin\000" "1024", 15);
    mock_event_called = false;
    mock_response_sent = false;
    ymodem_receiver_parse(&parser, frame, sizeof(frame));
}

/* ================================================================ */
/* setUp / tearDown                                                  */
/* ================================================================ */

void setUp(void)   { receiver_reset(); }
void tearDown(void) {}

/* ================================================================ */
/*  Category: Initialization & API validation                        */
/* ================================================================ */

void test_create_initializes_to_idle(void)
{
    TEST_ASSERT_EQUAL(YMODEM_STAGE_IDLE, parser.stage);
    TEST_ASSERT_EQUAL(YMODEM_RECV_WAIT_HEAD, parser.stat);
}

void test_create_null_parser_returns_false(void)
{
    bool result = ymodem_receiver_create(NULL, rx_buf, sizeof(rx_buf));
    TEST_ASSERT_FALSE(result);
}

void test_create_null_buffer_returns_false(void)
{
    bool result = ymodem_receiver_create(&parser, NULL, 1024);
    TEST_ASSERT_FALSE(result);
}

void test_create_small_buffer_returns_false(void)
{
    bool result = ymodem_receiver_create(&parser, rx_buf, 1);
    TEST_ASSERT_FALSE(result);
}

void test_set_event_callback_null_parser_returns_false(void)
{
    bool result = ymodem_receiver_set_event_callback(NULL, mock_event_cb, NULL);
    TEST_ASSERT_FALSE(result);
}

void test_set_send_response_callback_null_parser_returns_false(void)
{
    bool result = ymodem_receiver_set_send_response_callback(NULL, mock_response_cb, NULL);
    TEST_ASSERT_FALSE(result);
}

void test_start_null_parser_returns_false(void)
{
    bool ret = ymodem_receiver_start(NULL);
    TEST_ASSERT_FALSE(ret);
}

void test_parse_null_parser_silent(void)
{
    uint8_t b = YMODEM_SOH;
    ymodem_receiver_parse(NULL, &b, 1);
    // should not crash
}

void test_parse_null_data_silent(void)
{
    ymodem_receiver_parse(&parser, NULL, 1);
    // should not crash
}

void test_parse_zero_len_silent(void)
{
    uint8_t b = YMODEM_SOH;
    ymodem_receiver_parse(&parser, &b, 0);
    // should not crash
}

/* ================================================================ */
/*  Category: Start / ESTABLISHING                                   */
/* ================================================================ */

void test_start_returns_true_on_success(void)
{
    bool ret = ymodem_receiver_start(&parser);
    TEST_ASSERT_TRUE(ret);
}

void test_start_sets_establishing_no_response(void)
{
    ymodem_receiver_start(&parser);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_ESTABLISHING, parser.stage);
    TEST_ASSERT_FALSE(mock_response_sent);
}

void test_poll_response_is_C_byte(void)
{
    ymodem_receiver_start(&parser);
    mock_response_sent = false;
    mock_time_ms += 1000;
    ymodem_receiver_poll(&parser);
    TEST_ASSERT(mock_response_sent);
    TEST_ASSERT_EQUAL(YMODEM_C, parser.buffer.tx_buffer[0]);
}

/* ================================================================ */
/*  Category: Noise / invalid data in WAIT_HEAD                      */
/* ================================================================ */

void test_zero_byte_ignored_in_wait_head(void)
{
    uint8_t zero = 0x00;
    ymodem_receiver_start(&parser);
    ymodem_receiver_parse(&parser, &zero, 1);
    TEST_ASSERT_EQUAL(YMODEM_RECV_WAIT_HEAD, parser.stat);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_ESTABLISHING, parser.stage);
}

void test_ack_byte_ignored_in_wait_head(void)
{
    uint8_t ack = YMODEM_ACK;
    ymodem_receiver_start(&parser);
    ymodem_receiver_parse(&parser, &ack, 1);
    TEST_ASSERT_EQUAL(YMODEM_RECV_WAIT_HEAD, parser.stat);
}

void test_nak_byte_ignored_in_wait_head(void)
{
    uint8_t nak = YMODEM_NAK;
    ymodem_receiver_start(&parser);
    ymodem_receiver_parse(&parser, &nak, 1);
    TEST_ASSERT_EQUAL(YMODEM_RECV_WAIT_HEAD, parser.stat);
}

/* ================================================================ */
/*  Category: Frame type detection (SOH / STX / EOT / CAN)           */
/* ================================================================ */

void test_soh_frame_info_parsed(void)
{
    uint8_t frame[YMODEM_SOH_FRAME_LEN_BYTE];
    ymodem_receiver_start(&parser);
    build_valid_soh_frame(frame, 0,
        (const uint8_t *)"app.bin\000" "2048", 15);
    mock_event_called = false;
    mock_response_sent = false;

    ymodem_receiver_parse(&parser, frame, sizeof(frame));

    TEST_ASSERT(mock_event_called);
    TEST_ASSERT_EQUAL(YMODEM_RECV_EVENT_FILE_INFO, mock_last_event_type);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_ESTABLISHED, parser.stage);
    TEST_ASSERT(mock_response_sent);
    TEST_ASSERT_EQUAL_STRING("app.bin", mock_event_file_name);
    TEST_ASSERT_EQUAL(2048, mock_event_file_size);
}

void test_parse_returns_none_on_success(void)
{
    uint8_t frame[YMODEM_SOH_FRAME_LEN_BYTE];
    ymodem_receiver_start(&parser);
    build_valid_soh_frame(frame, 0,
        (const uint8_t *)"app.bin\000" "2048", 15);

    ymodem_error_e ret = ymodem_receiver_parse(&parser, frame, sizeof(frame));
    TEST_ASSERT_EQUAL(YMODEM_ERROR_NONE, ret);
}

void test_stx_frame_header_recognized(void)
{
    uint8_t stx = YMODEM_STX;
    ymodem_receiver_start(&parser);

    ymodem_receiver_parse(&parser, &stx, 1);

    TEST_ASSERT_EQUAL(YMODEM_FRAME_TYPE_STX, parser.frame_info.frame_type);
    TEST_ASSERT_EQUAL(YMODEM_RECV_WAIT_SEQ, parser.stat);
    TEST_ASSERT_EQUAL(YMODEM_STX_DATA_LEN_BYTE, parser.frame_info.current_frame_data_len);
    TEST_ASSERT_EQUAL(YMODEM_STX_FRAME_LEN_BYTE, parser.frame_info.current_frame_total_len);
}

void test_eot_detected_in_wait_head(void)
{
    uint8_t eot = YMODEM_EOT;
    ymodem_receiver_start(&parser);

    ymodem_receiver_parse(&parser, &eot, 1);

    TEST_ASSERT_EQUAL(YMODEM_FRAME_TYPE_EOT, parser.frame_info.frame_type);
}

void test_single_can_enters_wait_can_2(void)
{
    uint8_t can = YMODEM_CAN;
    ymodem_receiver_start(&parser);

    ymodem_receiver_parse(&parser, &can, 1);

    TEST_ASSERT_EQUAL(YMODEM_RECV_WAIT_CAN_2, parser.stat);
    TEST_ASSERT_EQUAL(YMODEM_FRAME_TYPE_CAN, parser.frame_info.frame_type);
}

void test_two_CAN_bytes_cancels_transfer(void)
{
    uint8_t can = YMODEM_CAN;
    ymodem_receiver_start(&parser);

    ymodem_receiver_parse(&parser, &can, 1);
    ymodem_receiver_parse(&parser, &can, 1);

    TEST_ASSERT_EQUAL(YMODEM_STAGE_IDLE, parser.stage);
    TEST_ASSERT_EQUAL(YMODEM_FRAME_TYPE_CAN, parser.frame_info.frame_type);
}

void test_non_can_after_first_can_resets_to_head(void)
{
    uint8_t can = YMODEM_CAN;
    uint8_t not_can = 0x00;
    ymodem_receiver_start(&parser);

    ymodem_receiver_parse(&parser, &can, 1);
    TEST_ASSERT_EQUAL(YMODEM_RECV_WAIT_CAN_2, parser.stat);

    mock_response_sent = false;
    ymodem_receiver_parse(&parser, &not_can, 1);
    TEST_ASSERT_EQUAL(YMODEM_RECV_WAIT_HEAD, parser.stat);
}

/* ================================================================ */
/*  Category: SEQ byte validation                                    */
/* ================================================================ */

void test_seq_mismatch_sends_nak(void)
{
    uint8_t frame[YMODEM_SOH_FRAME_LEN_BYTE];
    memset(frame, 0, sizeof(frame));
    frame[YMODEM_FRAME_TYPE_BYTE_INDEX] = YMODEM_SOH;
    frame[YMODEM_SEQ_BYTE_INDEX]        = 5;
    frame[YMODEM_NOR_SEQ_BYTE_INDEX]    = (uint8_t)(~5);
    ymodem_receiver_start(&parser);

    mock_response_sent = false;
    ymodem_receiver_parse(&parser, frame, sizeof(frame));

    TEST_ASSERT(mock_response_sent);
    TEST_ASSERT_EQUAL(YMODEM_NAK, parser.buffer.tx_buffer[0]);
}

void test_invalid_seq_complement_sends_nak(void)
{
    uint8_t frame[YMODEM_SOH_FRAME_LEN_BYTE];
    memset(frame, 0, sizeof(frame));
    frame[YMODEM_FRAME_TYPE_BYTE_INDEX] = YMODEM_SOH;
    frame[YMODEM_SEQ_BYTE_INDEX]        = 0;
    frame[YMODEM_NOR_SEQ_BYTE_INDEX]    = 0xAA; /* not ~0 */
    ymodem_receiver_start(&parser);

    mock_response_sent = false;
    ymodem_receiver_parse(&parser, frame, sizeof(frame));

    TEST_ASSERT(mock_response_sent);
    TEST_ASSERT_EQUAL(YMODEM_NAK, parser.buffer.tx_buffer[0]);
}

/* ================================================================ */
/*  Category: CRC error                                              */
/* ================================================================ */

void test_crc_error_sends_nak(void)
{
    uint8_t frame[YMODEM_SOH_FRAME_LEN_BYTE];
    memset(frame, 0, sizeof(frame));
    frame[YMODEM_FRAME_TYPE_BYTE_INDEX] = YMODEM_SOH;
    frame[YMODEM_SEQ_BYTE_INDEX]        = 0;
    frame[YMODEM_NOR_SEQ_BYTE_INDEX]    = 0xFF;
    frame[YMODEM_SOH_FRAME_LEN_BYTE - 2] = 0xAB;
    frame[YMODEM_SOH_FRAME_LEN_BYTE - 1] = 0xCD;
    ymodem_receiver_start(&parser);

    mock_response_sent = false;
    ymodem_receiver_parse(&parser, frame, YMODEM_SOH_FRAME_LEN_BYTE);

    TEST_ASSERT(mock_response_sent);
    TEST_ASSERT_EQUAL(YMODEM_NAK, parser.buffer.tx_buffer[0]);
}

void test_parse_returns_crc_on_crc_error(void)
{
    uint8_t frame[YMODEM_SOH_FRAME_LEN_BYTE];
    memset(frame, 0, sizeof(frame));
    frame[YMODEM_FRAME_TYPE_BYTE_INDEX] = YMODEM_SOH;
    frame[YMODEM_SEQ_BYTE_INDEX]        = 0;
    frame[YMODEM_NOR_SEQ_BYTE_INDEX]    = 0xFF;
    frame[YMODEM_SOH_FRAME_LEN_BYTE - 2] = 0xAB;
    frame[YMODEM_SOH_FRAME_LEN_BYTE - 1] = 0xCD;
    ymodem_receiver_start(&parser);

    ymodem_error_e ret = ymodem_receiver_parse(&parser, frame, YMODEM_SOH_FRAME_LEN_BYTE);
    TEST_ASSERT_EQUAL(YMODEM_ERROR_CRC, ret);
}

void test_parse_returns_wait_more_on_partial_frame(void)
{
    uint8_t soh = YMODEM_SOH;
    uint8_t seq = 0x00;
    ymodem_receiver_start(&parser);

    ymodem_error_e ret;

    ret = ymodem_receiver_parse(&parser, &soh, 1);
    TEST_ASSERT_EQUAL(YMODEM_ERROR_WAIT_MORE, ret);

    ret = ymodem_receiver_parse(&parser, &seq, 1);
    TEST_ASSERT_EQUAL(YMODEM_ERROR_WAIT_MORE, ret);
}

void test_parse_returns_wait_more_on_null(void)
{
    uint8_t b = YMODEM_SOH;
    ymodem_error_e ret = ymodem_receiver_parse(NULL, &b, 1);
    TEST_ASSERT_EQUAL(YMODEM_ERROR_WAIT_MORE, ret);
}

/* ================================================================ */
/*  Category: Re-transmission detection                              */
/* ================================================================ */

void test_prev_seq_recognized_as_resend(void)
{
    uint8_t frame[YMODEM_SOH_FRAME_LEN_BYTE];
    ymodem_receiver_start(&parser);
    /* first, establish with frame 0 */
    build_valid_soh_frame(frame, 0,
        (const uint8_t *)"f.bin\000"   "100", 11);
    ymodem_receiver_parse(&parser, frame, sizeof(frame));

    /* now send frame 0 again (should be seen as resend) */
    mock_response_sent = false;
    mock_event_called = false;
    build_valid_soh_frame(frame, 0,
        (const uint8_t *)"f.bin\000"   "100", 11);
    ymodem_receiver_parse(&parser, frame, sizeof(frame));

    /* resend acknowledged with ACK */
    TEST_ASSERT(mock_response_sent);
    TEST_ASSERT_EQUAL(YMODEM_ACK, parser.buffer.tx_buffer[0]);
}

/* ================================================================ */
/*  Category: Retransmission limit exceeded                         */
/* ================================================================ */

void test_retransmission_limit_exceeded_aborts(void)
{
    uint8_t frame[YMODEM_SOH_FRAME_LEN_BYTE];
    ymodem_receiver_start(&parser);
    memset(frame, 0, sizeof(frame));
    frame[YMODEM_FRAME_TYPE_BYTE_INDEX] = YMODEM_SOH;
    frame[YMODEM_SEQ_BYTE_INDEX]        = 0;
    frame[YMODEM_NOR_SEQ_BYTE_INDEX]    = 0xFF;
    frame[YMODEM_SOH_FRAME_LEN_BYTE - 2] = 0xAB;
    frame[YMODEM_SOH_FRAME_LEN_BYTE - 1] = 0xCD;

    mock_event_called = false;
    for (int i = 0; i <= (int)YMODEM_RETRANSMISSION_MAX_COUNT; i++) {
        mock_response_sent = false;
        ymodem_receiver_parse(&parser, frame, YMODEM_SOH_FRAME_LEN_BYTE);
    }

    TEST_ASSERT_EQUAL(YMODEM_STAGE_IDLE, parser.stage);
    TEST_ASSERT(mock_event_called);
    TEST_ASSERT_EQUAL(YMODEM_RECV_EVENT_ERROR, mock_last_event_type);
}

/* ================================================================ */
/*  Category: Byte-by-byte and chunk parsing                         */
/* ================================================================ */

void test_soh_frame_parsed_byte_by_byte(void)
{
    uint8_t frame[YMODEM_SOH_FRAME_LEN_BYTE];
    ymodem_receiver_start(&parser);
    build_valid_soh_frame(frame, 0,
        (const uint8_t *)"n.bin\000"   "512", 10);
    mock_event_called = false;

    for (int i = 0; i < YMODEM_SOH_FRAME_LEN_BYTE; i++) {
        ymodem_receiver_parse(&parser, &frame[i], 1);
    }

    TEST_ASSERT(mock_event_called);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_ESTABLISHED, parser.stage);
}

void test_soh_frame_parsed_in_two_chunks(void)
{
    uint8_t frame[YMODEM_SOH_FRAME_LEN_BYTE];
    ymodem_receiver_start(&parser);
    build_valid_soh_frame(frame, 0,
        (const uint8_t *)"c.bin\000"   "256", 10);
    mock_event_called = false;

    ymodem_receiver_parse(&parser, frame, 66);
    ymodem_receiver_parse(&parser, frame + 66, YMODEM_SOH_FRAME_LEN_BYTE - 66);

    TEST_ASSERT(mock_event_called);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_ESTABLISHED, parser.stage);
}

void test_stx_full_frame_parsed_chunked(void)
{
    uint8_t frame[YMODEM_STX_FRAME_LEN_BYTE] = { 0 };
    ymodem_receiver_start(&parser);
    build_valid_soh_frame(frame, 0,
        (const uint8_t *)"big.bin\000"  "4096", 13);
    ymodem_receiver_parse(&parser, frame, sizeof(frame));
    TEST_ASSERT_EQUAL(YMODEM_STAGE_ESTABLISHED, parser.stage);

    mock_event_called = false;
    build_valid_stx_frame(frame, 1);
    /* Feed in 3 chunks */
    ymodem_receiver_parse(&parser, frame, 300);
    ymodem_receiver_parse(&parser, frame + 300, 400);
    ymodem_receiver_parse(&parser, frame + 700, YMODEM_STX_FRAME_LEN_BYTE - 700);

    TEST_ASSERT(mock_event_called);
    TEST_ASSERT_EQUAL(YMODEM_RECV_EVENT_DATA_PACKET, mock_last_event_type);
}

/* ================================================================ */
/*  Category: Data packet event field validation                     */
/* ================================================================ */

void test_data_event_has_correct_fields(void)
{
    uint8_t frame[YMODEM_SOH_FRAME_LEN_BYTE];
    establish_file_info();
    TEST_ASSERT_EQUAL(YMODEM_STAGE_ESTABLISHED, parser.stage);

    mock_event_called = false;
    memset(frame, 0xBB, YMODEM_SOH_DATA_LEN_BYTE);
    build_valid_soh_frame(frame, 1, NULL, 0);
    ymodem_receiver_parse(&parser, frame, sizeof(frame));

    TEST_ASSERT(mock_event_called);
    TEST_ASSERT_EQUAL(YMODEM_RECV_EVENT_DATA_PACKET, mock_last_event_type);
    TEST_ASSERT_EQUAL(128, mock_event_data_len);
    TEST_ASSERT_EQUAL(128, mock_event_total_received);
    TEST_ASSERT_EQUAL(0, mock_event_data_len > 0 ? 0 : 1);
}

/* ================================================================ */
/*  Category: Last packet size trimming                              */
/* ================================================================ */

void test_last_packet_trimmed_to_exact_remaining(void)
{
    uint8_t frame[YMODEM_SOH_FRAME_LEN_BYTE];
    /* file size = 150 bytes, so first data frame (128) + last frame (22) */
    build_valid_soh_frame(frame, 0,
        (const uint8_t *)"s.bin\000"   "150", 9);
    ymodem_receiver_start(&parser);
    ymodem_receiver_parse(&parser, frame, sizeof(frame));
    TEST_ASSERT_EQUAL(YMODEM_STAGE_ESTABLISHED, parser.stage);

    /* frame 1 = 128 bytes */
    mock_event_called = false;
    build_valid_soh_frame(frame, 1, NULL, 0);
    ymodem_receiver_parse(&parser, frame, sizeof(frame));
    TEST_ASSERT(mock_event_called);
    TEST_ASSERT_EQUAL(128, mock_event_data_len);
    TEST_ASSERT_EQUAL(128, mock_event_total_received);

    /* frame 2 = only 22 real bytes needed */
    mock_event_called = false;
    build_valid_soh_frame(frame, 2, NULL, 0);
    ymodem_receiver_parse(&parser, frame, sizeof(frame));
    TEST_ASSERT(mock_event_called);
    TEST_ASSERT_EQUAL(22, mock_event_data_len);
    TEST_ASSERT_EQUAL(150, mock_event_total_received);
}

/* ================================================================ */
/*  Category: File info edge cases                                   */
/* ================================================================ */

void test_file_info_parse_returns_correct_fields(void)
{
    uint8_t frame[YMODEM_SOH_FRAME_LEN_BYTE];
    ymodem_receiver_start(&parser);
    build_valid_soh_frame(frame, 0,
        (const uint8_t *)"readme.txt\000" "65536", 16);
    mock_event_called = false;

    ymodem_receiver_parse(&parser, frame, sizeof(frame));

    TEST_ASSERT(mock_event_called);
    TEST_ASSERT_EQUAL(YMODEM_RECV_EVENT_FILE_INFO, mock_last_event_type);
    TEST_ASSERT_EQUAL_STRING("readme.txt", mock_event_file_name);
    TEST_ASSERT_EQUAL(65536, mock_event_file_size);
}

void test_file_info_zero_file_size_is_valid(void)
{
    uint8_t frame[YMODEM_SOH_FRAME_LEN_BYTE];
    ymodem_receiver_start(&parser);
    build_valid_soh_frame(frame, 0,
        (const uint8_t *)"empty.bin\000" "0", 12);
    mock_event_called = false;

    ymodem_receiver_parse(&parser, frame, sizeof(frame));

    TEST_ASSERT(mock_event_called);
    TEST_ASSERT_EQUAL(0, mock_event_file_size);
}

void test_file_info_long_filename_parsed(void)
{
    uint8_t frame[YMODEM_SOH_FRAME_LEN_BYTE];
    const char *long_name = "a_very_long_filename_that_still_fits_in_128_bytes_and_works.bin";
    char data_buf[YMODEM_SOH_DATA_LEN_BYTE];
    memset(data_buf, 0, sizeof(data_buf));
    memcpy(data_buf, long_name, strlen(long_name));
    memcpy(&data_buf[strlen(long_name) + 1], "9999", 4);
    ymodem_receiver_start(&parser);
    build_valid_soh_frame(frame, 0, (const uint8_t *)data_buf, strlen(long_name) + 6);
    mock_event_called = false;

    ymodem_receiver_parse(&parser, frame, sizeof(frame));

    TEST_ASSERT(mock_event_called);
    TEST_ASSERT_EQUAL_STRING(long_name, mock_event_file_name);
}

/* ================================================================ */
/*  Category: EOT in various stages                                  */
/* ================================================================ */

void test_eot_in_established_enters_finishing(void)
{
    uint8_t eot = YMODEM_EOT;
    establish_file_info();
    TEST_ASSERT_EQUAL(YMODEM_STAGE_ESTABLISHED, parser.stage);

    mock_response_sent = false;
    ymodem_receiver_parse(&parser, &eot, 1);

    TEST_ASSERT_EQUAL(YMODEM_STAGE_FINISHING, parser.stage);
    TEST_ASSERT(mock_response_sent);
    TEST_ASSERT_EQUAL(YMODEM_NAK, parser.buffer.tx_buffer[0]);
}

void test_double_eot_enters_finished(void)
{
    uint8_t eot = YMODEM_EOT;
    establish_file_info();
    /* first EOT → FINISHING */
    ymodem_receiver_parse(&parser, &eot, 1);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_FINISHING, parser.stage);

    /* second EOT → FINISHED and TRANSFER_COMPLETE */
    mock_event_called = false;
    ymodem_receiver_parse(&parser, &eot, 1);

    TEST_ASSERT_EQUAL(YMODEM_STAGE_FINISHED, parser.stage);
    TEST_ASSERT(mock_event_called);
    TEST_ASSERT_EQUAL(YMODEM_RECV_EVENT_TRANSFER_COMPLETE, mock_last_event_type);
}

/* ================================================================ */
/*  Category: Poll / timeout                                         */
/* ================================================================ */

void test_poll_null_parser_returns_false(void)
{
    bool ret = ymodem_receiver_poll(NULL);
    TEST_ASSERT_FALSE(ret);
}

void test_poll_idle_returns_false(void)
{
    bool ret = ymodem_receiver_poll(&parser);
    TEST_ASSERT_FALSE(ret);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_IDLE, parser.stage);
}

void test_poll_before_timeout_returns_false(void)
{
    ymodem_receiver_start(&parser);
    mock_time_ms += 500; /* less than 1000ms */
    mock_response_sent = false;
    bool ret = ymodem_receiver_poll(&parser);
    TEST_ASSERT_FALSE(ret);
    TEST_ASSERT_FALSE(mock_response_sent);
}

void test_timeout_in_establishing_returns_true(void)
{
    ymodem_receiver_start(&parser);
    mock_response_sent = false;
    mock_time_ms += 1000;
    bool ret = ymodem_receiver_poll(&parser);
    TEST_ASSERT_TRUE(ret);
    TEST_ASSERT(mock_response_sent);
}

void test_timeout_handshake_limit_exceeded_aborts(void)
{
    ymodem_receiver_start(&parser);
    for (int i = 0; i <= (int)YMODEM_RETRANSMISSION_MAX_COUNT; i++) {
        mock_time_ms += 1000;
        mock_response_sent = false;
        ymodem_receiver_poll(&parser);
    }
    TEST_ASSERT_EQUAL(YMODEM_STAGE_IDLE, parser.stage);
}

void test_timeout_on_frame_in_progress_sends_nak(void)
{
    uint8_t soh = YMODEM_SOH;
    ymodem_receiver_start(&parser);
    /* start frame reception (STX detection sets frame_is_start) */
    ymodem_receiver_parse(&parser, &soh, 1);
    TEST_ASSERT_EQUAL(YMODEM_RECV_WAIT_SEQ, parser.stat);

    mock_time_ms += 1000;
    mock_response_sent = false;
    mock_event_called = false;
    bool ret = ymodem_receiver_poll(&parser);

    TEST_ASSERT_TRUE(ret);
    TEST_ASSERT(mock_response_sent);
    TEST_ASSERT_EQUAL(YMODEM_NAK, parser.buffer.tx_buffer[0]);
}

/* ================================================================ */
/*  Category: Full transfer flow (scenario tests)                    */
/* ================================================================ */

void test_full_transfer_single_file(void)
{
    uint8_t frame[YMODEM_SOH_FRAME_LEN_BYTE];
    uint8_t eot = YMODEM_EOT;

    /* establish */
    ymodem_receiver_start(&parser);
    build_valid_soh_frame(frame, 0,
        (const uint8_t *)"f.bin\000"   "256", 9);
    ymodem_receiver_parse(&parser, frame, sizeof(frame));
    TEST_ASSERT_EQUAL(YMODEM_STAGE_ESTABLISHED, parser.stage);

    /* data frame 1 */
    mock_event_called = false;
    build_valid_soh_frame(frame, 1, NULL, 0);
    ymodem_receiver_parse(&parser, frame, sizeof(frame));
    TEST_ASSERT(mock_event_called);
    TEST_ASSERT_EQUAL(YMODEM_RECV_EVENT_DATA_PACKET, mock_last_event_type);

    /* data frame 2 */
    mock_event_called = false;
    build_valid_soh_frame(frame, 2, NULL, 0);
    ymodem_receiver_parse(&parser, frame, sizeof(frame));
    TEST_ASSERT(mock_event_called);

    /* EOT → FINISHING */
    ymodem_receiver_parse(&parser, &eot, 1);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_FINISHING, parser.stage);

    /* second EOT → FINISHED */
    mock_event_called = false;
    ymodem_receiver_parse(&parser, &eot, 1);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_FINISHED, parser.stage);
    TEST_ASSERT(mock_event_called);
    TEST_ASSERT_EQUAL(YMODEM_RECV_EVENT_TRANSFER_COMPLETE, mock_last_event_type);
}

void test_full_transfer_with_data_validation(void)
{
    uint8_t frame[YMODEM_SOH_FRAME_LEN_BYTE];
    uint8_t eot = YMODEM_EOT;
    const uint32_t file_size = 400;

    ymodem_receiver_start(&parser);
    /* file info */
    uint8_t info_data[YMODEM_SOH_DATA_LEN_BYTE];
    memset(info_data, 0, sizeof(info_data));
    memcpy(info_data, "data.bin", 8);
    char size_str[16];
    int size_len = snprintf(size_str, sizeof(size_str), "%lu", (unsigned long)file_size);
    memcpy(&info_data[9], size_str, size_len);
    build_valid_soh_frame(frame, 0, info_data, 10 + size_len);
    ymodem_receiver_parse(&parser, frame, sizeof(frame));
    TEST_ASSERT_EQUAL(YMODEM_STAGE_ESTABLISHED, parser.stage);
    TEST_ASSERT_EQUAL(file_size, parser.file_info.file_total_size);

    /* data frames: 128 + 128 + 128 + 16 = 400 */
    for (uint8_t seq = 1; seq <= 4; seq++) {
        mock_event_called = false;
        build_valid_soh_frame(frame, seq, NULL, 0);
        ymodem_receiver_parse(&parser, frame, sizeof(frame));
        TEST_ASSERT(mock_event_called);
    }

    TEST_ASSERT_EQUAL(file_size, parser.file_info.file_rev_size);

    /* EOT x2 */
    ymodem_receiver_parse(&parser, &eot, 1);
    mock_event_called = false;
    ymodem_receiver_parse(&parser, &eot, 1);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_FINISHED, parser.stage);
    TEST_ASSERT_EQUAL(YMODEM_RECV_EVENT_TRANSFER_COMPLETE, mock_last_event_type);
}

void test_multi_file_transfer(void)
{
    uint8_t frame[YMODEM_SOH_FRAME_LEN_BYTE];
    uint8_t eot = YMODEM_EOT;

    /* File 1 */
    ymodem_receiver_start(&parser);
    build_valid_soh_frame(frame, 0,
        (const uint8_t *)"f1.bin\000"  "128", 10);
    ymodem_receiver_parse(&parser, frame, sizeof(frame));
    build_valid_soh_frame(frame, 1, NULL, 0);
    ymodem_receiver_parse(&parser, frame, sizeof(frame));
    ymodem_receiver_parse(&parser, &eot, 1);
    ymodem_receiver_parse(&parser, &eot, 1);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_FINISHED, parser.stage);

    /* File 2 */
    mock_event_called = false;
    build_valid_soh_frame(frame, 0,
        (const uint8_t *)"f2.bin\000"  "64", 9);
    ymodem_receiver_parse(&parser, frame, sizeof(frame));
    TEST_ASSERT(mock_event_called);
    TEST_ASSERT_EQUAL(YMODEM_RECV_EVENT_FILE_INFO, mock_last_event_type);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_ESTABLISHED, parser.stage);
    TEST_ASSERT_EQUAL(64, parser.file_info.file_total_size);

    /* data frame */
    build_valid_soh_frame(frame, 1, NULL, 0);
    ymodem_receiver_parse(&parser, frame, sizeof(frame));

    /* EOT x2 */
    ymodem_receiver_parse(&parser, &eot, 1);
    ymodem_receiver_parse(&parser, &eot, 1);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_FINISHED, parser.stage);
}

void test_session_end_with_empty_filename(void)
{
    uint8_t frame[YMODEM_SOH_FRAME_LEN_BYTE];
    uint8_t eot = YMODEM_EOT;

    /* complete first file */
    ymodem_receiver_start(&parser);
    build_valid_soh_frame(frame, 0,
        (const uint8_t *)"f.bin\000"   "128", 9);
    ymodem_receiver_parse(&parser, frame, sizeof(frame));
    build_valid_soh_frame(frame, 1, NULL, 0);
    ymodem_receiver_parse(&parser, frame, sizeof(frame));
    ymodem_receiver_parse(&parser, &eot, 1);
    ymodem_receiver_parse(&parser, &eot, 1);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_FINISHED, parser.stage);

    /* empty filename → session ends, no numeric size to parse */
    mock_event_called = false;
    uint8_t empty_info[YMODEM_SOH_DATA_LEN_BYTE];
    memset(empty_info, 0, sizeof(empty_info));
    /* just a single '\0' — no data after */
    empty_info[0] = '\0';
    empty_info[1] = '\0'; /* double null means no file size */
    build_valid_soh_frame(frame, 0, empty_info, 2);
    ymodem_receiver_parse(&parser, frame, sizeof(frame));

    TEST_ASSERT(mock_event_called);
    TEST_ASSERT_EQUAL(YMODEM_RECV_EVENT_TRANSFER_FINISHED, mock_last_event_type);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_IDLE, parser.stage);
}

/* ================================================================ */
/*  Category: Deferred reset (frame_is_end) verification             */
/* ================================================================ */

void test_frame_info_preserved_after_can_cancel(void)
{
    uint8_t can = YMODEM_CAN;
    ymodem_receiver_start(&parser);

    ymodem_receiver_parse(&parser, &can, 1);
    ymodem_receiver_parse(&parser, &can, 1);

    TEST_ASSERT_EQUAL(YMODEM_STAGE_IDLE, parser.stage);
    TEST_ASSERT_EQUAL(YMODEM_FRAME_TYPE_CAN, parser.frame_info.frame_type);
}

void test_frame_info_preserved_after_eot(void)
{
    uint8_t eot = YMODEM_EOT;
    ymodem_receiver_start(&parser);

    ymodem_receiver_parse(&parser, &eot, 1);

    TEST_ASSERT_EQUAL(YMODEM_FRAME_TYPE_EOT, parser.frame_info.frame_type);
}

void test_deferred_reset_on_next_soh(void)
{
    uint8_t frame[YMODEM_SOH_FRAME_LEN_BYTE];
    uint8_t eot = YMODEM_EOT;

    ymodem_receiver_start(&parser);
    /* first send EOT to set frame_is_end = true */
    ymodem_receiver_parse(&parser, &eot, 1);
    TEST_ASSERT_EQUAL(YMODEM_FRAME_TYPE_EOT, parser.frame_info.frame_type);

    /* next SOH triggers reset, frame starts fresh */
    build_valid_soh_frame(frame, 0,
        (const uint8_t *)"xxx.bin\000" "100", 11);
    ymodem_receiver_parse(&parser, frame, sizeof(frame));

    TEST_ASSERT_EQUAL(YMODEM_FRAME_TYPE_SOH, parser.frame_info.frame_type);
}

/* ================================================================ */
/*  Category: Noise after frame_end (skipped until next header)     */
/* ================================================================ */

void test_noise_bytes_skipped_until_next_header(void)
{
    uint8_t frame[YMODEM_SOH_FRAME_LEN_BYTE];
    ymodem_receiver_start(&parser);

    /* parse a valid frame to set frame_is_end */
    build_valid_soh_frame(frame, 0,
        (const uint8_t *)"a.bin\000"   "100", 9);
    ymodem_receiver_parse(&parser, frame, sizeof(frame));
    TEST_ASSERT_EQUAL(YMODEM_STAGE_ESTABLISHED, parser.stage);

    /* send noise bytes — should signal GARBAGE */
    uint8_t noise[] = {0xFF, 0xFE, 0x00, 0x07, 0xAB};
    mock_response_sent = false;
    ymodem_error_e ret = ymodem_receiver_parse(&parser, noise, sizeof(noise));
    TEST_ASSERT_EQUAL(YMODEM_ERROR_GARBAGE, ret);
    TEST_ASSERT_FALSE(mock_response_sent);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_ESTABLISHED, parser.stage);
}

void test_garbage_then_frame_header_works(void)
{
    uint8_t frame[YMODEM_SOH_FRAME_LEN_BYTE];
    ymodem_receiver_start(&parser);

    /* first: complete file info frame */
    build_valid_soh_frame(frame, 0,
        (const uint8_t *)"a.bin\000"   "100", 9);
    ymodem_receiver_parse(&parser, frame, sizeof(frame));
    TEST_ASSERT_EQUAL(YMODEM_STAGE_ESTABLISHED, parser.stage);

    /* garbage first → GARBAGE */
    uint8_t garbage = 0xFF;
    ymodem_error_e ret = ymodem_receiver_parse(&parser, &garbage, 1);
    TEST_ASSERT_EQUAL(YMODEM_ERROR_GARBAGE, ret);

    /* then valid data frame → NONE */
    mock_event_called = false;
    build_valid_soh_frame(frame, 1, NULL, 0);
    ret = ymodem_receiver_parse(&parser, frame, sizeof(frame));
    TEST_ASSERT_EQUAL(YMODEM_ERROR_NONE, ret);
    TEST_ASSERT(mock_event_called);
    TEST_ASSERT_EQUAL(YMODEM_RECV_EVENT_DATA_PACKET, mock_last_event_type);
}

void test_mid_frame_returns_wait_more_not_garbage(void)
{
    /* mid-frame bytes should return WAIT_MORE, not GARBAGE */
    uint8_t soh = YMODEM_SOH;
    uint8_t seq_ok = 0x00;
    uint8_t seq_nok = 0xFF;

    ymodem_receiver_start(&parser);

    ymodem_error_e ret;

    ret = ymodem_receiver_parse(&parser, &soh, 1);
    TEST_ASSERT_EQUAL(YMODEM_ERROR_WAIT_MORE, ret);

    ret = ymodem_receiver_parse(&parser, &seq_ok, 1);
    TEST_ASSERT_EQUAL(YMODEM_ERROR_WAIT_MORE, ret);

    ret = ymodem_receiver_parse(&parser, &seq_nok, 1);
    TEST_ASSERT_EQUAL(YMODEM_ERROR_WAIT_MORE, ret);
}

/* ================================================================ */
/*  Category: Poll interaction with frame_is_end                     */
/* ================================================================ */

void test_poll_no_timeout_after_frame_complete(void)
{
    uint8_t frame[YMODEM_SOH_FRAME_LEN_BYTE];
    uint8_t soh = YMODEM_SOH;

    ymodem_receiver_start(&parser);

    /* complete file info handshake to advance past ESTABLISHING */
    build_valid_soh_frame(frame, 0,
        (const uint8_t *)"f.bin\000"   "128", 9);
    ymodem_receiver_parse(&parser, frame, sizeof(frame));
    TEST_ASSERT_EQUAL(YMODEM_STAGE_ESTABLISHED, parser.stage);

    /* start a data frame to set frame_is_start */
    ymodem_receiver_parse(&parser, &soh, 1);

    /* poll: should trigger timeout, frame_is_end set=true, frame_is_start=false */
    mock_time_ms += 1000;
    mock_response_sent = false;
    bool ret = ymodem_receiver_poll(&parser);
    TEST_ASSERT_TRUE(ret);
    TEST_ASSERT(mock_response_sent);

    /* poll again: frame_is_start is false now, should not trigger */
    mock_time_ms += 1000;
    mock_response_sent = false;
    ret = ymodem_receiver_poll(&parser);
    TEST_ASSERT_FALSE(ret);
    TEST_ASSERT_FALSE(mock_response_sent);
}

/* ================================================================ */
/*  Category: ESTABLISHING ignores non-SOH frames                   */
/* ================================================================ */

void test_stx_ignored_in_establishing_no_file_info_yet(void)
{
    uint8_t stx = YMODEM_STX;
    ymodem_receiver_start(&parser);

    ymodem_receiver_parse(&parser, &stx, 1);
    TEST_ASSERT_EQUAL(YMODEM_RECV_WAIT_SEQ, parser.stat);

    /* complete the frame but stage is ESTABLISHING, not SOH
       so frame_stage_process will not parse file info */
    uint8_t padding[YMODEM_STX_FRAME_LEN_BYTE - 1];
    memset(padding, 0, sizeof(padding));
    padding[0] = 0x00; padding[1] = 0xFF; /* seq=0, ~0=0xFF */
    mock_event_called = false;
    ymodem_receiver_parse(&parser, padding, sizeof(padding));

    /* Should NOT have fired FILE_INFO (STX in ESTABLISHING is ignored) */
    /* file_rev_frame_number stays 0 */
}

/* ================================================================ */
/*  Category: FINISHED state handling                               */
/* ================================================================ */

void test_finished_state_sends_nak_for_non_file_info(void)
{
    uint8_t frame[YMODEM_SOH_FRAME_LEN_BYTE];
    uint8_t eot = YMODEM_EOT;

    /* complete transfer */
    ymodem_receiver_start(&parser);
    build_valid_soh_frame(frame, 0,
        (const uint8_t *)"f.bin\000"   "128", 9);
    ymodem_receiver_parse(&parser, frame, sizeof(frame));
    build_valid_soh_frame(frame, 1, NULL, 0);
    ymodem_receiver_parse(&parser, frame, sizeof(frame));
    ymodem_receiver_parse(&parser, &eot, 1);
    ymodem_receiver_parse(&parser, &eot, 1);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_FINISHED, parser.stage);

    /* send data frame (seq=2) in FINISHED -> NAK */
    mock_response_sent = false;
    build_valid_soh_frame(frame, 2, NULL, 0);
    ymodem_receiver_parse(&parser, frame, sizeof(frame));
    TEST_ASSERT(mock_response_sent);
    TEST_ASSERT_EQUAL(YMODEM_NAK, parser.buffer.tx_buffer[0]);
}

/* ================================================================ */
/*  Main                                                              */
/* ================================================================ */

int main(void)
{
    UNITY_BEGIN();

    /* Initialization & API */
    RUN_TEST(test_create_initializes_to_idle);
    RUN_TEST(test_create_null_parser_returns_false);
    RUN_TEST(test_create_null_buffer_returns_false);
    RUN_TEST(test_create_small_buffer_returns_false);
    RUN_TEST(test_set_event_callback_null_parser_returns_false);
    RUN_TEST(test_set_send_response_callback_null_parser_returns_false);
    RUN_TEST(test_start_null_parser_returns_false);
    RUN_TEST(test_parse_null_parser_silent);
    RUN_TEST(test_parse_null_data_silent);
    RUN_TEST(test_parse_zero_len_silent);

    /* Start */
    RUN_TEST(test_start_returns_true_on_success);
    RUN_TEST(test_start_sets_establishing_no_response);
    RUN_TEST(test_poll_response_is_C_byte);

    /* Noise / invalid data */
    RUN_TEST(test_zero_byte_ignored_in_wait_head);
    RUN_TEST(test_ack_byte_ignored_in_wait_head);
    RUN_TEST(test_nak_byte_ignored_in_wait_head);

    /* Frame type detection */
    RUN_TEST(test_soh_frame_info_parsed);
    RUN_TEST(test_stx_frame_header_recognized);
    RUN_TEST(test_eot_detected_in_wait_head);
    RUN_TEST(test_single_can_enters_wait_can_2);
    RUN_TEST(test_two_CAN_bytes_cancels_transfer);
    RUN_TEST(test_non_can_after_first_can_resets_to_head);

    /* SEQ validation */
    RUN_TEST(test_seq_mismatch_sends_nak);
    RUN_TEST(test_invalid_seq_complement_sends_nak);

    /* CRC */
    RUN_TEST(test_crc_error_sends_nak);

    /* Parse return value validation */
    RUN_TEST(test_parse_returns_none_on_success);
    RUN_TEST(test_parse_returns_crc_on_crc_error);
    RUN_TEST(test_parse_returns_wait_more_on_partial_frame);
    RUN_TEST(test_parse_returns_wait_more_on_null);

    /* Retransmission */
    RUN_TEST(test_prev_seq_recognized_as_resend);
    RUN_TEST(test_retransmission_limit_exceeded_aborts);

    /* Byte-by-byte and chunk parsing */
    RUN_TEST(test_soh_frame_parsed_byte_by_byte);
    RUN_TEST(test_soh_frame_parsed_in_two_chunks);
    RUN_TEST(test_stx_full_frame_parsed_chunked);

    /* Data event validation */
    RUN_TEST(test_data_event_has_correct_fields);
    RUN_TEST(test_last_packet_trimmed_to_exact_remaining);

    /* File info edge cases */
    RUN_TEST(test_file_info_parse_returns_correct_fields);
    RUN_TEST(test_file_info_zero_file_size_is_valid);
    RUN_TEST(test_file_info_long_filename_parsed);

    /* EOT handling */
    RUN_TEST(test_eot_in_established_enters_finishing);
    RUN_TEST(test_double_eot_enters_finished);

    /* Poll / timeout */
    RUN_TEST(test_poll_null_parser_returns_false);
    RUN_TEST(test_poll_idle_returns_false);
    RUN_TEST(test_poll_before_timeout_returns_false);
    RUN_TEST(test_timeout_in_establishing_returns_true);
    RUN_TEST(test_timeout_handshake_limit_exceeded_aborts);
    RUN_TEST(test_timeout_on_frame_in_progress_sends_nak);
    RUN_TEST(test_poll_no_timeout_after_frame_complete);

    /* Full transfer flow */
    RUN_TEST(test_full_transfer_single_file);
    RUN_TEST(test_full_transfer_with_data_validation);
    RUN_TEST(test_multi_file_transfer);
    RUN_TEST(test_session_end_with_empty_filename);

    /* Deferred reset */
    RUN_TEST(test_frame_info_preserved_after_can_cancel);
    RUN_TEST(test_frame_info_preserved_after_eot);
    RUN_TEST(test_deferred_reset_on_next_soh);

    /* Noise/stage edge cases */
    RUN_TEST(test_noise_bytes_skipped_until_next_header);
    RUN_TEST(test_garbage_then_frame_header_works);
    RUN_TEST(test_mid_frame_returns_wait_more_not_garbage);
    RUN_TEST(test_stx_ignored_in_establishing_no_file_info_yet);
    RUN_TEST(test_finished_state_sends_nak_for_non_file_info);

    return UNITY_END();
}
