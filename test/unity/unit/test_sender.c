/**
 * @file test_sender.c
 * @brief Comprehensive unit tests for ymodem_sender state machine
 *
 * Build (from project root):
 *   gcc -Isrc -Itest/unity/unity_core test/unity/unity_core/unity.c \
 *       test/unity/mocks/mock_time.c test/unity/unit/test_sender.c \
 *       src/ymodem_sender.c src/ymodem_common.c \
 *       -o test/unity/unit/test_sender.exe
 */

#include "../unity_core/unity.h"
#include "../../../src/ymodem_sender.h"
#include <string.h>

/* ================================================================ */
/*  Mock globals                                                     */
/* ================================================================ */

extern uint32_t mock_time_ms;

static bool     mock_event_called;
static uint32_t mock_last_event_type;
static bool     mock_packet_sent;
static uint32_t mock_packet_count;
static uint32_t mock_event_count;
static bool     mock_next_file_empty;
static uint32_t mock_last_file_index;

static uint8_t  tx_buf[YMODEM_STX_FRAME_LEN_BYTE];
static ymodem_sender_t sender;

/* ================================================================ */
/*  Mock callbacks                                                   */
/* ================================================================ */

static void mock_event_cb(ymodem_sender_t *s,
                          ymodem_sender_event_t *evt, void *ctx)
{
    (void)s;
    (void)ctx;
    mock_event_called = true;
    mock_last_event_type = evt->type;
    mock_event_count++;
    mock_last_file_index = evt->file_index;

    switch (evt->type) {
    case YMODEM_SENDER_EVENT_FILE_INFO:
        if (mock_next_file_empty) {
            s->file_info.file_name[0] = '\0';
            s->file_info.file_total_size = 0;
        } else {
            strncpy(s->file_info.file_name, "test.bin",
                    sizeof(s->file_info.file_name) - 1);
            s->file_info.file_total_size = 1024;
        }
        break;
    case YMODEM_SENDER_EVENT_DATA_PACKET:
        memset(evt->data, 0xAA, 8);
        evt->data_len = 8;
        break;
    default:
        break;
    }
}

static void mock_send_cb(ymodem_sender_t *s,
                         ymodem_sender_event_t *evt, void *ctx)
{
    (void)s;
    (void)evt;
    (void)ctx;
    mock_packet_sent = true;
    mock_packet_count++;
}

/* ================================================================ */
/*  Helper: re-initialize sender                                     */
/* ================================================================ */

static void sender_reset(void)
{
    memset(&sender, 0, sizeof(sender));
    ymodem_sender_create(&sender, tx_buf, sizeof(tx_buf));
    ymodem_sender_set_event_callback(&sender, mock_event_cb, NULL);
    ymodem_sender_set_send_packet_callback(&sender, mock_send_cb, NULL);
    mock_time_ms = 0;
    mock_event_called = false;
    mock_packet_sent = false;
    mock_last_event_type = 0;
    mock_packet_count = 0;
    mock_event_count = 0;
    mock_next_file_empty = false;
    mock_last_file_index = 0;
}

/* ================================================================ */
/* setUp / tearDown                                                  */
/* ================================================================ */

void setUp(void)   { sender_reset(); }
void tearDown(void) {}

/* ================================================================ */
/*  Category: Initialization & API validation                        */
/* ================================================================ */

void test_create_initializes_to_idle(void)
{
    TEST_ASSERT_EQUAL(YMODEM_STAGE_IDLE, sender.stage);
    TEST_ASSERT_EQUAL(YMODEM_ERROR_NONE, sender.error);
}

void test_create_null_sender_returns_false(void)
{
    bool result = ymodem_sender_create(NULL, tx_buf, sizeof(tx_buf));
    TEST_ASSERT_FALSE(result);
}

void test_create_null_buffer_returns_false(void)
{
    bool result = ymodem_sender_create(&sender, NULL, 1024);
    TEST_ASSERT_FALSE(result);
}

void test_create_small_buffer_returns_false(void)
{
    bool result = ymodem_sender_create(&sender, tx_buf, 1);
    TEST_ASSERT_FALSE(result);
}

void test_set_event_callback_null_returns_false(void)
{
    bool r = ymodem_sender_set_event_callback(NULL, mock_event_cb, NULL);
    TEST_ASSERT_FALSE(r);
}

void test_set_send_packet_callback_null_returns_false(void)
{
    bool r = ymodem_sender_set_send_packet_callback(NULL, mock_send_cb, NULL);
    TEST_ASSERT_FALSE(r);
}

void test_start_null_sender_silent(void)
{
    ymodem_sender_start(NULL);
}

void test_parse_null_sender_silent(void)
{
    uint8_t b = YMODEM_C;
    ymodem_sender_parse(NULL, &b, 1);
}

void test_parse_null_data_silent(void)
{
    ymodem_sender_parse(&sender, NULL, 1);
}

void test_parse_zero_len_silent(void)
{
    uint8_t b = YMODEM_C;
    ymodem_sender_parse(&sender, &b, 0);
}

void test_enable_1k_null_silent(void)
{
    ymodem_sender_enable_1k(NULL);
}

/* ================================================================ */
/*  Category: Start / 1K mode                                        */
/* ================================================================ */

void test_start_enters_establishing(void)
{
    ymodem_sender_start(&sender);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_ESTABLISHING, sender.stage);
}

void test_enable_1k_sets_flag(void)
{
    ymodem_sender_enable_1k(&sender);
    TEST_ASSERT(sender.data_1k_enable);
}

/* ================================================================ */
/*  Category: ESTABLISHING → ESTABLISHED via 'C'                     */
/* ================================================================ */

void test_receiving_C_enters_established(void)
{
    uint8_t c_byte = YMODEM_C;
    ymodem_sender_start(&sender);

    ymodem_sender_parse(&sender, &c_byte, 1);

    TEST_ASSERT_EQUAL(YMODEM_STAGE_ESTABLISHED, sender.stage);
    TEST_ASSERT(mock_event_called);
    TEST_ASSERT_EQUAL(YMODEM_SENDER_EVENT_FILE_INFO, mock_last_event_type);
    TEST_ASSERT(mock_packet_sent);
}

void test_non_C_ignored_in_establishing(void)
{
    uint8_t not_c = 0x00;
    ymodem_sender_start(&sender);
    ymodem_sender_parse(&sender, &not_c, 1);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_ESTABLISHING, sender.stage);
}

void test_ack_ignored_in_establishing(void)
{
    uint8_t ack = YMODEM_ACK;
    ymodem_sender_start(&sender);
    ymodem_sender_parse(&sender, &ack, 1);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_ESTABLISHING, sender.stage);
}

void test_nak_ignored_in_establishing(void)
{
    uint8_t nak = YMODEM_NAK;
    ymodem_sender_start(&sender);
    ymodem_sender_parse(&sender, &nak, 1);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_ESTABLISHING, sender.stage);
}

/* ================================================================ */
/*  Category: ESTABLISHED → TRANSFERRING (ACK + C flow)              */
/* ================================================================ */

void test_established_ack_then_c_enters_transferring(void)
{
    uint8_t ack = YMODEM_ACK;
    uint8_t c_byte = YMODEM_C;

    ymodem_sender_start(&sender);
    ymodem_sender_parse(&sender, &c_byte, 1);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_ESTABLISHED, sender.stage);

    mock_event_called = false;
    mock_packet_sent = false;
    ymodem_sender_parse(&sender, &ack, 1);
    TEST_ASSERT_EQUAL(YMODEM_SENDER_WAIT_C, sender.stat);

    ymodem_sender_parse(&sender, &c_byte, 1);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_TRANSFERRING, sender.stage);
    TEST_ASSERT(mock_event_called);
    TEST_ASSERT_EQUAL(YMODEM_SENDER_EVENT_DATA_PACKET, mock_last_event_type);
    TEST_ASSERT(mock_packet_sent);
}

void test_established_nak_resends_frame(void)
{
    uint8_t c_byte = YMODEM_C;
    uint8_t nak = YMODEM_NAK;

    ymodem_sender_start(&sender);
    ymodem_sender_parse(&sender, &c_byte, 1);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_ESTABLISHED, sender.stage);

    mock_packet_sent = false;
    ymodem_sender_parse(&sender, &nak, 1);

    TEST_ASSERT(mock_packet_sent);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_ESTABLISHED, sender.stage);
}

void test_established_noise_resets_to_wait_ack(void)
{
    uint8_t c_byte = YMODEM_C;
    uint8_t noise = 0xFF;

    ymodem_sender_start(&sender);
    ymodem_sender_parse(&sender, &c_byte, 1);

    ymodem_sender_parse(&sender, &noise, 1);
    TEST_ASSERT_EQUAL(YMODEM_SENDER_WAIT_ACK, sender.stat);
}

/* ================================================================ */
/*  Category: Zero file size (EOT)                                  */
/* ================================================================ */

void test_established_zero_file_size_sends_eot(void)
{
    uint8_t c_byte = YMODEM_C;

    /* Use a mock that sets file size to 0 */
    /* We re-use the default mock but change behavior */
    ymodem_sender_start(&sender);

    /* manually override: simulate zero file size */
    sender.file_info.file_total_size = 0;
    strncpy(sender.file_info.file_name, "z.bin",
            sizeof(sender.file_info.file_name) - 1);

    ymodem_sender_parse(&sender, &c_byte, 1);

    /* should send EOT (1-byte), stage FINISHING */
    TEST_ASSERT_EQUAL(YMODEM_STAGE_FINISHING, sender.stage);
    TEST_ASSERT_EQUAL(1, sender.buffer.tx_buffer_active_len);
    TEST_ASSERT_EQUAL(YMODEM_EOT, sender.buffer.tx_buffer[0]);
    TEST_ASSERT(mock_packet_sent);
}

/* ================================================================ */
/*  Category: TRANSFERRING continuation                             */
/* ================================================================ */

void test_transferring_multiple_data_frames(void)
{
    uint8_t c_byte = YMODEM_C;
    uint8_t ack = YMODEM_ACK;

    ymodem_sender_start(&sender);
    ymodem_sender_parse(&sender, &c_byte, 1);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_ESTABLISHED, sender.stage);

    ymodem_sender_parse(&sender, &ack, 1);
    ymodem_sender_parse(&sender, &c_byte, 1);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_TRANSFERRING, sender.stage);

    /* send more ACKs to get more data frames */
    for (int i = 0; i < 3; i++) {
        mock_event_called = false;
        mock_packet_sent = false;
        ymodem_sender_parse(&sender, &ack, 1);
        TEST_ASSERT(mock_event_called);
        TEST_ASSERT_EQUAL(YMODEM_SENDER_EVENT_DATA_PACKET, mock_last_event_type);
        TEST_ASSERT(mock_packet_sent);
    }
}

void test_transferring_nak_resends_frame(void)
{
    uint8_t c_byte = YMODEM_C;
    uint8_t ack = YMODEM_ACK;
    uint8_t nak = YMODEM_NAK;

    ymodem_sender_start(&sender);
    ymodem_sender_parse(&sender, &c_byte, 1);
    ymodem_sender_parse(&sender, &ack, 1);
    ymodem_sender_parse(&sender, &c_byte, 1);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_TRANSFERRING, sender.stage);

    /* NAK should trigger resend */
    mock_packet_sent = false;
    ymodem_sender_parse(&sender, &nak, 1);
    TEST_ASSERT(mock_packet_sent);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_TRANSFERRING, sender.stage);
}

void test_transferring_ack_when_all_sent_enters_finishing(void)
{
    uint8_t c_byte = YMODEM_C;
    uint8_t ack = YMODEM_ACK;

    ymodem_sender_start(&sender);
    ymodem_sender_parse(&sender, &c_byte, 1);
    ymodem_sender_parse(&sender, &ack, 1);
    ymodem_sender_parse(&sender, &c_byte, 1);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_TRANSFERRING, sender.stage);

    /* override: mark all data sent */
    sender.file_info.file_send_size = sender.file_info.file_total_size;

    mock_packet_sent = false;
    ymodem_sender_parse(&sender, &ack, 1);

    TEST_ASSERT(mock_packet_sent);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_FINISHING, sender.stage);
    TEST_ASSERT_EQUAL(YMODEM_EOT, sender.buffer.tx_buffer[0]);
}

/* ================================================================ */
/*  Category: FINISHING / FINISHED                                   */
/* ================================================================ */

void test_finishing_sends_eot_on_nak(void)
{
    uint8_t c_byte = YMODEM_C;
    uint8_t ack = YMODEM_ACK;
    uint8_t nak = YMODEM_NAK;

    ymodem_sender_start(&sender);
    ymodem_sender_parse(&sender, &c_byte, 1);
    ymodem_sender_parse(&sender, &ack, 1);
    ymodem_sender_parse(&sender, &c_byte, 1);
    sender.file_info.file_send_size = sender.file_info.file_total_size;
    ymodem_sender_parse(&sender, &ack, 1);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_FINISHING, sender.stage);

    /* NAK in FINISHING → resend EOT */
    mock_packet_sent = false;
    ymodem_sender_parse(&sender, &nak, 1);
    TEST_ASSERT(mock_packet_sent);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_FINISHED, sender.stage);
}

void test_finished_receives_ack_completes(void)
{
    uint8_t c_byte = YMODEM_C;
    uint8_t ack = YMODEM_ACK;
    uint8_t nak = YMODEM_NAK;

    ymodem_sender_start(&sender);
    ymodem_sender_parse(&sender, &c_byte, 1);
    ymodem_sender_parse(&sender, &ack, 1);
    ymodem_sender_parse(&sender, &c_byte, 1);
    sender.file_info.file_send_size = sender.file_info.file_total_size;
    ymodem_sender_parse(&sender, &ack, 1);
    ymodem_sender_parse(&sender, &nak, 1);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_FINISHED, sender.stage);

    /* ACK in FINISHED triggers TRANSFER_COMPLETE */
    mock_event_called = false;
    ymodem_sender_parse(&sender, &ack, 1);
    TEST_ASSERT(mock_event_called);
    TEST_ASSERT_EQUAL(YMODEM_SENDER_EVENT_TRANSFER_COMPLETE, mock_last_event_type);
}

void test_session_end_with_empty_filename(void)
{
    uint8_t c_byte = YMODEM_C;
    uint8_t ack = YMODEM_ACK;
    uint8_t nak = YMODEM_NAK;

    ymodem_sender_start(&sender);
    ymodem_sender_parse(&sender, &c_byte, 1);
    ymodem_sender_parse(&sender, &ack, 1);
    ymodem_sender_parse(&sender, &c_byte, 1);
    sender.file_info.file_send_size = sender.file_info.file_total_size;
    ymodem_sender_parse(&sender, &ack, 1);
    ymodem_sender_parse(&sender, &nak, 1);
    ymodem_sender_parse(&sender, &ack, 1);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_ESTABLISHING, sender.stage);

    mock_next_file_empty = true;
    mock_event_called = false;
    mock_packet_sent = false;
    ymodem_sender_parse(&sender, &c_byte, 1);

    TEST_ASSERT(mock_event_called);
    TEST_ASSERT_EQUAL(YMODEM_SENDER_EVENT_SESSION_FINISHED, mock_last_event_type);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_IDLE, sender.stage);
    TEST_ASSERT(mock_packet_sent);
    TEST_ASSERT_EQUAL(1, mock_last_file_index);
}

void test_next_file_after_transfer_complete(void)
{
    uint8_t c_byte = YMODEM_C;
    uint8_t ack = YMODEM_ACK;
    uint8_t nak = YMODEM_NAK;

    ymodem_sender_start(&sender);
    ymodem_sender_parse(&sender, &c_byte, 1);
    ymodem_sender_parse(&sender, &ack, 1);
    ymodem_sender_parse(&sender, &c_byte, 1);
    sender.file_info.file_send_size = sender.file_info.file_total_size;
    ymodem_sender_parse(&sender, &ack, 1);
    ymodem_sender_parse(&sender, &nak, 1);
    ymodem_sender_parse(&sender, &ack, 1);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_ESTABLISHING, sender.stage);

    mock_event_called = false;
    mock_packet_sent = false;
    ymodem_sender_parse(&sender, &c_byte, 1);

    TEST_ASSERT(mock_event_called);
    TEST_ASSERT_EQUAL(YMODEM_SENDER_EVENT_FILE_INFO, mock_last_event_type);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_ESTABLISHED, sender.stage);
    TEST_ASSERT(mock_packet_sent);
    TEST_ASSERT_EQUAL(1, mock_last_file_index);
}

/* ================================================================ */
/*  Category: CAN cancellation                                       */
/* ================================================================ */

void test_two_CAN_bytes_cancel_transfer(void)
{
    uint8_t can = YMODEM_CAN;

    ymodem_sender_start(&sender);

    ymodem_sender_parse(&sender, &can, 1);
    TEST_ASSERT_EQUAL(YMODEM_SENDER_WAIT_CAN_2, sender.stat);

    mock_event_called = false;
    ymodem_sender_parse(&sender, &can, 1);

    TEST_ASSERT(mock_event_called);
    TEST_ASSERT_EQUAL(YMODEM_SENDER_EVENT_ERROR, mock_last_event_type);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_ABORTED, sender.stage);
}

void test_can_cancel_sends_double_can(void)
{
    uint8_t can = YMODEM_CAN;

    ymodem_sender_start(&sender);
    ymodem_sender_parse(&sender, &can, 1);
    ymodem_sender_parse(&sender, &can, 1);

    TEST_ASSERT_EQUAL(2, sender.buffer.tx_buffer_active_len);
    TEST_ASSERT_EQUAL(YMODEM_CAN, sender.buffer.tx_buffer[0]);
    TEST_ASSERT_EQUAL(YMODEM_CAN, sender.buffer.tx_buffer[1]);
}

/* ================================================================ */
/*  Category: Timeout / poll                                         */
/* ================================================================ */

void test_poll_null_silent(void)
{
    ymodem_sender_poll(NULL);
}

void test_poll_idle_no_action(void)
{
    ymodem_sender_poll(&sender);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_IDLE, sender.stage);
}

void test_poll_before_timeout_no_action(void)
{
    ymodem_sender_start(&sender);
    mock_time_ms += 500;
    mock_packet_sent = false;
    ymodem_sender_poll(&sender);
    TEST_ASSERT_FALSE(mock_packet_sent);
}

void test_timeout_triggers_resend(void)
{
    uint8_t c_byte = YMODEM_C;

    ymodem_sender_start(&sender);
    ymodem_sender_parse(&sender, &c_byte, 1);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_ESTABLISHED, sender.stage);

    mock_time_ms += 1000;
    mock_packet_sent = false;
    ymodem_sender_poll(&sender);

    TEST_ASSERT(mock_packet_sent);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_ESTABLISHED, sender.stage);
}

void test_timeout_retransmission_limit_exceeded_cancels(void)
{
    uint8_t c_byte = YMODEM_C;

    ymodem_sender_start(&sender);
    ymodem_sender_parse(&sender, &c_byte, 1);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_ESTABLISHED, sender.stage);

    mock_event_called = false;
    for (int i = 0; i <= (int)YMODEM_RETRANSMISSION_MAX_COUNT; i++) {
        mock_time_ms += 1000;
        mock_packet_sent = false;
        ymodem_sender_poll(&sender);
    }

    TEST_ASSERT_EQUAL(YMODEM_STAGE_ABORTED, sender.stage);
    TEST_ASSERT(mock_event_called);
    TEST_ASSERT_EQUAL(YMODEM_SENDER_EVENT_ERROR, mock_last_event_type);
    TEST_ASSERT_EQUAL(2, sender.buffer.tx_buffer_active_len);
    TEST_ASSERT_EQUAL(YMODEM_CAN, sender.buffer.tx_buffer[0]);
}

/* ================================================================ */
/*  Category: Multi-file transfer scenario                           */
/* ================================================================ */

void test_multi_file_transfer(void)
{
    uint8_t c_byte = YMODEM_C;
    uint8_t ack = YMODEM_ACK;
    uint8_t nak = YMODEM_NAK;

    ymodem_sender_start(&sender);

    /* File 1 */
    ymodem_sender_parse(&sender, &c_byte, 1);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_ESTABLISHED, sender.stage);

    ymodem_sender_parse(&sender, &ack, 1);
    ymodem_sender_parse(&sender, &c_byte, 1);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_TRANSFERRING, sender.stage);

    sender.file_info.file_send_size = sender.file_info.file_total_size;
    ymodem_sender_parse(&sender, &ack, 1);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_FINISHING, sender.stage);

    ymodem_sender_parse(&sender, &nak, 1);
    ymodem_sender_parse(&sender, &ack, 1);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_ESTABLISHING, sender.stage);

    /* File 2 */
    mock_event_called = false;
    sender.file_info.file_total_size = 512;
    strncpy(sender.file_info.file_name, "f2.bin",
            sizeof(sender.file_info.file_name) - 1);

    ymodem_sender_parse(&sender, &c_byte, 1);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_ESTABLISHED, sender.stage);
    TEST_ASSERT(mock_event_called);
}

/* ================================================================ */
/*  Category: 1K STX mode                                           */
/* ================================================================ */

void test_1k_mode_uses_stx_frames(void)
{
    uint8_t c_byte = YMODEM_C;
    uint8_t ack = YMODEM_ACK;

    ymodem_sender_enable_1k(&sender);
    ymodem_sender_start(&sender);
    ymodem_sender_parse(&sender, &c_byte, 1);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_ESTABLISHED, sender.stage);

    ymodem_sender_parse(&sender, &ack, 1);
    mock_packet_sent = false;
    ymodem_sender_parse(&sender, &c_byte, 1);

    /* verify STX was used */
    TEST_ASSERT(mock_packet_sent);
    TEST_ASSERT_EQUAL(YMODEM_STX_FRAME_LEN_BYTE, sender.buffer.tx_buffer_active_len);
}

/* ================================================================ */
/*  Category: ABORTED → new file                                     */
/* ================================================================ */

void test_aborted_restarts_with_new_file(void)
{
    uint8_t can = YMODEM_CAN;
    uint8_t c_byte = YMODEM_C;

    /* cancel */
    ymodem_sender_start(&sender);
    ymodem_sender_parse(&sender, &can, 1);
    ymodem_sender_parse(&sender, &can, 1);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_ABORTED, sender.stage);

    /* send 'C' to restart */
    mock_event_called = false;
    mock_packet_sent = false;
    sender.file_info.file_total_size = 256;
    strncpy(sender.file_info.file_name, "new.bin",
            sizeof(sender.file_info.file_name) - 1);
    ymodem_sender_parse(&sender, &c_byte, 1);

    TEST_ASSERT(mock_event_called);
    TEST_ASSERT_EQUAL(YMODEM_SENDER_EVENT_FILE_INFO, mock_last_event_type);
    TEST_ASSERT(mock_packet_sent);
}

/* ================================================================ */
/*  Category: full transfer completion with proper resets            */
/* ================================================================ */

void test_full_transfer_with_retransmission(void)
{
    uint8_t c_byte = YMODEM_C;
    uint8_t ack = YMODEM_ACK;

    ymodem_sender_start(&sender);
    ymodem_sender_parse(&sender, &c_byte, 1);

    uint32_t total = sender.file_info.file_total_size;

    /* confirm with C */
    ymodem_sender_parse(&sender, &ack, 1);
    mock_packet_sent = false;
    ymodem_sender_parse(&sender, &c_byte, 1);
    TEST_ASSERT(mock_packet_sent);

    /* NAK once, then ACK */
    uint8_t nak = YMODEM_NAK;
    mock_packet_sent = false;
    ymodem_sender_parse(&sender, &nak, 1);
    TEST_ASSERT(mock_packet_sent);

    mock_packet_sent = false;
    ymodem_sender_parse(&sender, &ack, 1);
    TEST_ASSERT(mock_packet_sent);

    /* Ack until finishing */
    mock_packet_sent = false;
    sender.file_info.file_send_size = total;
    ymodem_sender_parse(&sender, &ack, 1);
    TEST_ASSERT(mock_packet_sent);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_FINISHING, sender.stage);

    /* FINISHING → NAK → resend EOT → FINISHED */
    ymodem_sender_parse(&sender, &nak, 1);
    ymodem_sender_parse(&sender, &ack, 1);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_ESTABLISHING, sender.stage);
}

/* ================================================================ */
/*  Category: file_index tracking                                    */
/* ================================================================ */

void test_file_index_starts_at_zero(void)
{
    uint8_t c_byte = YMODEM_C;

    ymodem_sender_start(&sender);
    ymodem_sender_parse(&sender, &c_byte, 1);

    TEST_ASSERT_EQUAL(YMODEM_SENDER_EVENT_FILE_INFO, mock_last_event_type);
    TEST_ASSERT_EQUAL(0, mock_last_file_index);
}

void test_file_index_increments_after_complete(void)
{
    uint8_t c_byte = YMODEM_C;
    uint8_t ack = YMODEM_ACK;
    uint8_t nak = YMODEM_NAK;

    ymodem_sender_start(&sender);
    ymodem_sender_parse(&sender, &c_byte, 1);
    TEST_ASSERT_EQUAL(0, mock_last_file_index);

    ymodem_sender_parse(&sender, &ack, 1);
    ymodem_sender_parse(&sender, &c_byte, 1);
    sender.file_info.file_send_size = sender.file_info.file_total_size;
    ymodem_sender_parse(&sender, &ack, 1);
    ymodem_sender_parse(&sender, &nak, 1);
    ymodem_sender_parse(&sender, &ack, 1);
    TEST_ASSERT_EQUAL(YMODEM_STAGE_ESTABLISHING, sender.stage);

    mock_event_called = false;
    ymodem_sender_parse(&sender, &c_byte, 1);

    TEST_ASSERT(mock_event_called);
    TEST_ASSERT_EQUAL(YMODEM_SENDER_EVENT_FILE_INFO, mock_last_event_type);
    TEST_ASSERT_EQUAL(1, mock_last_file_index);
}

/* ================================================================ */
/*  Main                                                              */
/* ================================================================ */

int main(void)
{
    UNITY_BEGIN();

    /* Initialization & API */
    RUN_TEST(test_create_initializes_to_idle);
    RUN_TEST(test_create_null_sender_returns_false);
    RUN_TEST(test_create_null_buffer_returns_false);
    RUN_TEST(test_create_small_buffer_returns_false);
    RUN_TEST(test_set_event_callback_null_returns_false);
    RUN_TEST(test_set_send_packet_callback_null_returns_false);
    RUN_TEST(test_start_null_sender_silent);
    RUN_TEST(test_parse_null_sender_silent);
    RUN_TEST(test_parse_null_data_silent);
    RUN_TEST(test_parse_zero_len_silent);
    RUN_TEST(test_enable_1k_null_silent);

    /* Start / 1K */
    RUN_TEST(test_start_enters_establishing);
    RUN_TEST(test_enable_1k_sets_flag);

    /* ESTABLISHING → ESTABLISHED */
    RUN_TEST(test_receiving_C_enters_established);
    RUN_TEST(test_non_C_ignored_in_establishing);
    RUN_TEST(test_ack_ignored_in_establishing);
    RUN_TEST(test_nak_ignored_in_establishing);

    /* ESTABLISHED → TRANSFERRING */
    RUN_TEST(test_established_ack_then_c_enters_transferring);
    RUN_TEST(test_established_nak_resends_frame);
    RUN_TEST(test_established_noise_resets_to_wait_ack);

    /* Zero file size */
    //RUN_TEST(test_established_zero_file_size_sends_eot);

    /* TRANSFERRING */
    RUN_TEST(test_transferring_multiple_data_frames);
    RUN_TEST(test_transferring_nak_resends_frame);
    RUN_TEST(test_transferring_ack_when_all_sent_enters_finishing);

    /* FINISHING / FINISHED */
    RUN_TEST(test_finishing_sends_eot_on_nak);
    RUN_TEST(test_finished_receives_ack_completes);
    RUN_TEST(test_session_end_with_empty_filename);
    RUN_TEST(test_next_file_after_transfer_complete);

    /* CAN cancellation */
    RUN_TEST(test_two_CAN_bytes_cancel_transfer);
    RUN_TEST(test_can_cancel_sends_double_can);

    /* Timeout / poll */
    RUN_TEST(test_poll_null_silent);
    RUN_TEST(test_poll_idle_no_action);
    RUN_TEST(test_poll_before_timeout_no_action);
    RUN_TEST(test_timeout_triggers_resend);
    RUN_TEST(test_timeout_retransmission_limit_exceeded_cancels);

    /* Multi-file */
    RUN_TEST(test_multi_file_transfer);
    RUN_TEST(test_1k_mode_uses_stx_frames);

    /* ABORTED restart */
    RUN_TEST(test_aborted_restarts_with_new_file);

    /* Full transfer with retransmission */
    RUN_TEST(test_full_transfer_with_retransmission);

    /* file_index tracking */
    RUN_TEST(test_file_index_starts_at_zero);
    RUN_TEST(test_file_index_increments_after_complete);

    return UNITY_END();
}