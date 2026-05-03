/**
 * @file mock_time.c
 * @brief Mock for system_get_time_ms() — allows tests to control "time"
 *
 * Include this file in the test executable's compilation (not a header).
 * Mock callbacks are defined directly in each test file for simplicity.
 */

#include <stdint.h>

uint32_t mock_time_ms = 0;

uint32_t system_get_time_ms(void)
{
    return mock_time_ms;
}
