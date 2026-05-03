/**
 * @file test.c
 * @brief Unified Ymodem Test Entry Point
 *
 * Usage:
 *   ymodem_test.exe -s <COM port> <file path> [--1k] [-v] [-h]
 *   ymodem_test.exe -r <COM port> <save directory> [-v] [-h]
 *
 * Build from project root (ymodem_receiver/):
 *   MinGW/GCC:
 *     gcc -Isrc test/windows/test.c test/windows/ymodem_sender_test.c test/windows/ymodem_receiver_test.c src/ymodem_sender.c src/ymodem_receiver.c src/ymodem_common.c -o ymodem_test.exe
 *   MSVC:
 *     cl /Isrc test/windows/test.c test/windows/ymodem_sender_test.c test/windows/ymodem_receiver_test.c src/ymodem_sender.c src/ymodem_receiver.c src/ymodem_common.c /Fe:ymodem_test.exe
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <windows.h>

/* ================================================================
 *  Platform time (required by ymodem library)
 * ================================================================ */

uint32_t system_get_time_ms(void)
{
    return (uint32_t)GetTickCount64();
}

/* Run function declarations from test modules */
int ymodem_sender_test_run(const char* port_name, const char* file_path,
                           bool use_1k, bool verbose);

int ymodem_receiver_test_run(const char* port_name, const char* save_path,
                             bool verbose);

/* ================================================================
 *  Usage
 * ================================================================ */

static void print_usage(const char* prog)
{
    printf("Ymodem Unified Test Tool\n\n");
    printf("Usage:\n");
    printf("  %s -s <COM port> <file path> [--1k] [-v]\n", prog);
    printf("  %s -r <COM port> <save directory> [-v]\n\n", prog);
    printf("Modes:\n");
    printf("  -s    Sender mode  (send file to receiver)\n");
    printf("  -r    Receiver mode (receive file from sender)\n\n");
    printf("Sender options:\n");
    printf("  --1k  Enable 1024-byte data frame (default 128-byte)\n");
    printf("  -v    Verbose log mode\n\n");
    printf("Receiver options:\n");
    printf("  -v    Verbose log mode\n\n");
    printf("Interactive test flow:\n");
    printf("  1. Install virtual serial port software (com0com, VSPE, etc.)\n");
    printf("  2. Create a virtual serial port pair, e.g. COM15 <-> COM16\n");
    printf("  3. Start receiver: %s -r COM16 C:\\received\n", prog);
    printf("  4. Start sender:   %s -s COM15 firmware.bin\n", prog);
}

/* ================================================================
 *  Main
 * ================================================================ */

int main(int argc, char* argv[])
{
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    bool is_sender = false;
    int  arg_start = 1;

    if (strcmp(argv[1], "-s") == 0) {
        is_sender = true;
        arg_start = 2;
    }
    else if (strcmp(argv[1], "-r") == 0) {
        is_sender = false;
        arg_start = 2;
    }
    else if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        print_usage(argv[0]);
        return 0;
    }
    else {
        fprintf(stderr, "Error: must specify -s (sender) or -r (receiver)\n\n");
        print_usage(argv[0]);
        return 1;
    }

    if (argc - arg_start < 2) {
        fprintf(stderr, "Error: required arguments missing\n\n");
        print_usage(argv[0]);
        return 1;
    }

    const char* port_name = argv[arg_start];
    const char* path      = argv[arg_start + 1];
    bool verbose = false;

    if (is_sender) {
        bool use_1k = false;

        for (int i = arg_start + 2; i < argc; i++) {
            if (strcmp(argv[i], "--1k") == 0) {
                use_1k = true;
            }
            else if (strcmp(argv[i], "-v") == 0) {
                verbose = true;
            }
            else {
                fprintf(stderr, "Unknown option: %s\n", argv[i]);
                return 1;
            }
        }

        return ymodem_sender_test_run(port_name, path, use_1k, verbose);
    }
    else {
        for (int i = arg_start + 2; i < argc; i++) {
            if (strcmp(argv[i], "-v") == 0) {
                verbose = true;
            }
            else {
                fprintf(stderr, "Unknown option: %s\n", argv[i]);
                return 1;
            }
        }

        return ymodem_receiver_test_run(port_name, path, verbose);
    }
}
