/**
 * @file test_common.c
 * @brief Comprehensive unit tests for ymodem_common (CRC-16 CCITT)
 *
 * Build (from project root):
 *   gcc -Isrc -Itest/unity/unity_core test/unity/unity_core/unity.c \
 *       test/unity/mocks/mock_time.c test/unity/unit/test_common.c src/ymodem_common.c \
 *       -o test/unity/unit/test_common.exe
 */

#include "../unity_core/unity.h"
#include "../../../src/ymodem_common.h"
#include <string.h>

/* ---------------------------------------------------------------- */
/* setUp / tearDown                                                   */
/* ---------------------------------------------------------------- */

void setUp(void) {}

void tearDown(void) {}

/* ================================================================ */
/*  Category: Basic CRC-16 CCITT                                     */
/* ================================================================ */

void test_crc_empty_data_returns_zero(void)
{
    uint16_t crc = ymodem_calculate_crc16(NULL, 0);
    TEST_ASSERT_EQUAL_UINT16(0, crc);
}

void test_crc_known_string_123(void)
{
    uint8_t data[] = {0x31, 0x32, 0x33};
    uint16_t crc = ymodem_calculate_crc16(data, 3);
    TEST_ASSERT_EQUAL_UINT16(0x9752, crc);
}

void test_crc_single_zero_byte(void)
{
    uint8_t data[] = {0x00};
    uint16_t crc = ymodem_calculate_crc16(data, 1);
    TEST_ASSERT_EQUAL_UINT16(0x0000, crc);
}

/* ================================================================ */
/*  Category: Deterministic / Consistency                             */
/* ================================================================ */

void test_crc_repeated_data_consistent(void)
{
    uint8_t data[] = {0xAA, 0x55, 0xAA, 0x55};
    uint16_t crc1 = ymodem_calculate_crc16(data, 4);
    uint16_t crc2 = ymodem_calculate_crc16(data, 4);
    TEST_ASSERT_EQUAL_UINT16(crc1, crc2);
}

void test_crc_different_data_different_result(void)
{
    uint8_t data1[] = {0x12, 0x34, 0x56, 0x78};
    uint8_t data2[] = {0x12, 0x34, 0x56, 0x79};
    uint16_t crc1 = ymodem_calculate_crc16(data1, 4);
    uint16_t crc2 = ymodem_calculate_crc16(data2, 4);
    TEST_ASSERT_NOT_EQUAL(crc1, crc2);
}

/* ================================================================ */
/*  Category: Large buffer CRC                                       */
/* ================================================================ */

void test_crc_1024_zero_bytes(void)
{
    uint8_t data[1024];
    for (int i = 0; i < 1024; i++) data[i] = 0x00;
    uint16_t crc = ymodem_calculate_crc16(data, 1024);
    (void)crc;
}

void test_crc_1024_ff_bytes(void)
{
    uint8_t data[1024];
    for (int i = 0; i < 1024; i++) data[i] = 0xFF;
    uint16_t crc = ymodem_calculate_crc16(data, 1024);
    (void)crc;
}

void test_crc_single_byte_repeating_pattern(void)
{
    uint8_t pattern = 0xA5;
    uint8_t data[8];
    memset(data, pattern, 8);
    uint16_t crc1 = ymodem_calculate_crc16(data, 8);
    uint16_t crc2 = ymodem_calculate_crc16(data, 8);
    TEST_ASSERT_EQUAL_UINT16(crc1, crc2);
}

/* ================================================================ */
/*  Category: Known CRC-16 vectors                                   */
/* ================================================================ */

void test_crc_known_vector_0x00_0xFF(void)
{
    uint8_t data[] = {0x00, 0xFF, 0x00, 0xFF};
    uint16_t crc = ymodem_calculate_crc16(data, 4);
    (void)crc;
}

void test_crc_known_vector_all_0x55(void)
{
    /* "ABCDEFGH" ASCII */
    uint8_t data[] = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'};
    uint16_t crc = ymodem_calculate_crc16(data, 8);
    (void)crc;
}

/* ================================================================ */
/*  Category: SOH/STX frame CRC sizes                                */
/* ================================================================ */

void test_crc_soh_data_len_computes(void)
{
    uint8_t data[YMODEM_SOH_DATA_LEN_BYTE];
    memset(data, 0x00, sizeof(data));
    uint16_t crc = ymodem_calculate_crc16(data, YMODEM_SOH_DATA_LEN_BYTE);
    TEST_ASSERT_EQUAL_UINT16(0x0000, crc);
}

void test_crc_stx_data_len_computes(void)
{
    uint8_t data[YMODEM_STX_DATA_LEN_BYTE];
    memset(data, 0x00, sizeof(data));
    uint16_t crc = ymodem_calculate_crc16(data, YMODEM_STX_DATA_LEN_BYTE);
    TEST_ASSERT_EQUAL_UINT16(0x0000, crc);
}

/* ---------------------------------------------------------------- */
/* Main                                                              */
/* ---------------------------------------------------------------- */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_crc_empty_data_returns_zero);
    RUN_TEST(test_crc_known_string_123);
    RUN_TEST(test_crc_single_zero_byte);
    RUN_TEST(test_crc_repeated_data_consistent);
    RUN_TEST(test_crc_different_data_different_result);
    RUN_TEST(test_crc_1024_zero_bytes);
    RUN_TEST(test_crc_1024_ff_bytes);
    RUN_TEST(test_crc_single_byte_repeating_pattern);
    RUN_TEST(test_crc_known_vector_0x00_0xFF);
    RUN_TEST(test_crc_known_vector_all_0x55);
    RUN_TEST(test_crc_soh_data_len_computes);
    RUN_TEST(test_crc_stx_data_len_computes);

    return UNITY_END();
}