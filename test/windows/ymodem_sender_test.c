/**
 * @file ymodem_sender_test.c
 * @brief Windows Serial Ymodem Sender Test Program
 *
 * This file is part of the ymodem test suite. It is NOT a standalone program.
 * Use the unified entry point in test.c to run sender tests,
 * or define YMODEM_STANDALONE for a standalone build.
 *
 * Usage (via unified test.c):
 *   ymodem_test.exe -s <COM port> <file path> [--1k] [-v] [-h]
 *
 * Standalone build (MinGW/GCC):
 *   gcc -DYMODEM_STANDALONE -Isrc test/windows/ymodem_sender_test.c src/ymodem_sender.c src/ymodem_common.c -o ymodem_sender_test.exe
 *
 * Standalone build (MSVC):
 *   cl /DYMODEM_STANDALONE /Isrc test/windows/ymodem_sender_test.c src/ymodem_sender.c src/ymodem_common.c /Fe:ymodem_sender_test.exe
 *
 * Interactive test flow:
 *   1. Install virtual serial port software (com0com / Virtual Serial Port Emulator)
 *   2. Create a virtual serial port pair (e.g. COM15 <-> COM16)
 *   3. Open COM16 with XShell / Tera Term and enter Ymodem receive mode
 *   4. Run: ymodem_sender_test.exe COM15 test.bin
 */

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../src/ymodem_sender.h"

/* ================================================================
 *  Serial default parameters
 * ================================================================ */

#define TEST_SERIAL_BAUD_RATE   CBR_115200
#define TEST_SERIAL_BYTE_SIZE   8
#define TEST_SERIAL_PARITY      NOPARITY
#define TEST_SERIAL_STOP_BITS   ONESTOPBIT
#define TEST_SERIAL_READ_TO_MS  50
#define TEST_POLL_INTERVAL_MS   10

/* ================================================================
 *  Test context
 * ================================================================ */

typedef struct {
    HANDLE    h_com;
    FILE*     fp;
    const char* file_path;
    uint32_t  file_total_size;
    uint32_t  block_size;
    bool      transfer_done;
    bool      transfer_error;
    bool      verbose;
    uint32_t  current_file_index;
} test_ctx_t;

/* ================================================================
 *  Serial port operations
 * ================================================================ */

static HANDLE serial_open(const char* port_name)
{
    char path[64];
    snprintf(path, sizeof(path), "\\\\.\\%s", port_name);

    HANDLE h = CreateFileA(
        path,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL);

    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[FATAL] Cannot open serial port %s (error: %lu)\n", port_name, GetLastError());
        return INVALID_HANDLE_VALUE;
    }

    DCB dcb = {0};
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(h, &dcb)) {
        fprintf(stderr, "[FATAL] GetCommState failed\n");
        CloseHandle(h);
        return INVALID_HANDLE_VALUE;
    }

    dcb.BaudRate = TEST_SERIAL_BAUD_RATE;
    dcb.ByteSize = TEST_SERIAL_BYTE_SIZE;
    dcb.Parity   = TEST_SERIAL_PARITY;
    dcb.StopBits = TEST_SERIAL_STOP_BITS;
    dcb.fBinary  = TRUE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;

    if (!SetCommState(h, &dcb)) {
        fprintf(stderr, "[FATAL] SetCommState failed\n");
        CloseHandle(h);
        return INVALID_HANDLE_VALUE;
    }

    COMMTIMEOUTS timeouts = {0};
    timeouts.ReadIntervalTimeout         = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier  = 0;
    timeouts.ReadTotalTimeoutConstant    = TEST_SERIAL_READ_TO_MS;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant   = 1000;

    if (!SetCommTimeouts(h, &timeouts)) {
        fprintf(stderr, "[FATAL] SetCommTimeouts failed\n");
        CloseHandle(h);
        return INVALID_HANDLE_VALUE;
    }

    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    printf("[SERIAL] %s opened (%d bps, %dN%d)\n",
        port_name,
        TEST_SERIAL_BAUD_RATE == CBR_115200 ? 115200 : 9600,
        TEST_SERIAL_BYTE_SIZE,
        TEST_SERIAL_STOP_BITS == ONESTOPBIT ? 1 : 2);

    return h;
}

static void serial_close(HANDLE h)
{
    if (h != INVALID_HANDLE_VALUE) {
        PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
        CloseHandle(h);
    }
}

static bool serial_write(HANDLE h, const uint8_t* data, uint32_t len)
{
    DWORD written = 0;
    if (!WriteFile(h, data, len, &written, NULL) || written != len) {
        return false;
    }
    return true;
}

static int serial_read(HANDLE h, uint8_t* buf, uint32_t buf_size)
{
    DWORD n = 0;
    if (!ReadFile(h, buf, buf_size, &n, NULL)) {
        return -1;
    }
    return (int)n;
}

/* ================================================================
 *  Ymodem callback functions
 * ================================================================ */

static void on_ymodem_event(ymodem_sender_t* send,
    ymodem_sender_event_t* event, void* user_ctx)
{
    test_ctx_t* ctx = (test_ctx_t*)user_ctx;

    switch (event->type) {

    case YMODEM_SENDER_EVENT_FILE_INFO: {
        if (event->file_index != ctx->current_file_index) {
            send->file_info.file_name[0] = '\0';
            send->file_info.file_total_size = 0;
            printf("[EVENT] FILE_INFO  no more files, ending session\n");
        } else {
            const char* fname = strrchr(ctx->file_path, '\\');
            if (fname) fname++; else fname = ctx->file_path;
            strncpy(send->file_info.file_name, fname, sizeof(send->file_info.file_name) - 1);
            send->file_info.file_total_size = ctx->file_total_size;

            printf("[EVENT] FILE_INFO  idx=%u name=\"%s\" size=%u bytes\n",
                event->file_index, send->file_info.file_name, ctx->file_total_size);
        }
        break;
    }

    case YMODEM_SENDER_EVENT_DATA_PACKET: {
        uint32_t offset = event->data_seq * ctx->block_size;
        uint32_t remaining = ctx->file_total_size - offset;
        uint32_t to_read = (remaining < ctx->block_size) ? remaining : ctx->block_size;

        if (fseek(ctx->fp, (long)offset, SEEK_SET) != 0) {
            fprintf(stderr, "[ERROR] fseek offset=%u failed\n", offset);
            ctx->transfer_error = true;
            return;
        }

        uint32_t n = (uint32_t)fread(event->data, 1, to_read, ctx->fp);
        event->data_len = n;

        if (ctx->verbose) {
            printf("[DATA] seq=%u offset=%u len=%u/%u remaining=%u\n",
                event->data_seq, offset, n, ctx->block_size,
                remaining > n ? remaining - n : 0);
        }
        else {
            printf("\r[DATA] Progress: %u / %u bytes (%.1f%%)",
                offset + n, ctx->file_total_size,
                100.0f * (offset + n) / ctx->file_total_size);
            fflush(stdout);
        }
        break;
    }

    case YMODEM_SENDER_EVENT_TRANSFER_COMPLETE:
        printf("\n[EVENT] TRANSFER_COMPLETE\n");
        if (ctx->fp) {
            fclose(ctx->fp);
            ctx->fp = NULL;
        }
        ctx->transfer_done = true;
        break;

    case YMODEM_SENDER_EVENT_SESSION_FINISHED:
        printf("[EVENT] SESSION_FINISHED\n");
        break;

    case YMODEM_SENDER_EVENT_ERROR:
        fprintf(stderr, "\n[EVENT] ERROR - Transfer aborted\n");
        ctx->transfer_error = true;
        break;

    default:
        break;
    }
}

static void on_send_packet(ymodem_sender_t* send,
    ymodem_sender_event_t* send_event, void* user_ctx)
{
    test_ctx_t* ctx = (test_ctx_t*)user_ctx;

    uint32_t len = send->buffer.tx_buffer_active_len;

    if (ctx->verbose) {
        printf("[SEND] %u bytes: ", len);
        for (uint32_t i = 0; i < 6 && i < len; i++) {
            printf("%02X ", send->buffer.tx_buffer[i]);
        }
        if (len > 6) printf("...");
        printf("\n");
    }

    if (!serial_write(ctx->h_com, send->buffer.tx_buffer, len)) {
        fprintf(stderr, "[ERROR] Serial write failed\n");
        ctx->transfer_error = true;
    }
}

/* ================================================================
 *  Helper functions
 * ================================================================ */

static const char* stage_to_str(ymodem_stage_e stage)
{
    switch (stage) {
    case YMODEM_STAGE_IDLE:         return "IDLE";
    case YMODEM_STAGE_ESTABLISHING: return "ESTABLISHING";
    case YMODEM_STAGE_ESTABLISHED:  return "ESTABLISHED";
    case YMODEM_STAGE_TRANSFERRING: return "TRANSFERRING";
    case YMODEM_STAGE_FINISHING:    return "FINISHING";
    case YMODEM_STAGE_FINISHED:     return "FINISHED";
    case YMODEM_STAGE_ABORTED:      return "ABORTED";
    default:                        return "UNKNOWN";
    }
}

static void print_usage(const char* prog)
{
    printf("Ymodem Sender Test Tool\n");
    printf("Usage: %s <COM port> <file path> [options]\n\n", prog);
    printf("Arguments:\n");
    printf("  <COM port>    Serial port name, e.g. COM15\n");
    printf("  <file path>   Full path of file to send\n\n");
    printf("Options:\n");
    printf("  --1k          Enable 1024-byte data frame (default 128-byte)\n");
    printf("  -v            verbose log mode\n");
    printf("  -h            Show this help\n\n");
    printf("Examples:\n");
    printf("  %s COM15 firmware.bin\n", prog);
    printf("  %s COM15 firmware.bin --1k\n", prog);
    printf("  %s COM15 firmware.bin --1k -v\n", prog);
    printf("\n");
    printf("Interactive test flow:\n");
    printf("  1. Install virtual serial port software (com0com, VSPE, etc.)\n");
    printf("  2. Create a virtual serial port pair, e.g. COM15 <-> COM16\n");
    printf("  3. Open COM16 with XShell/Tera Term (115200 8N1)\n");
    printf("  4. Start Ymodem receive in the terminal\n");
    printf("  5. Run this program to send the file\n");
}

/* ================================================================
 *  Sender test run function (callable from unified test.c)
 * ================================================================ */

int ymodem_sender_test_run(const char* port_name, const char* file_path,
                           bool use_1k, bool verbose)
{
    /* ---- 1. Open file, get size ---- */

    FILE* fp = fopen(file_path, "rb");
    if (!fp) {
        fprintf(stderr, "[FATAL] Cannot open file: %s\n", file_path);
        return 1;
    }

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    rewind(fp);

    if (fsize <= 0) {
        fprintf(stderr, "[FATAL] File is empty or cannot get size\n");
        fclose(fp);
        return 1;
    }

    printf("[FILE] %s (%ld bytes)\n", file_path, fsize);

    /* ---- 2. Open serial port ---- */

    HANDLE h_com = serial_open(port_name);
    if (h_com == INVALID_HANDLE_VALUE) {
        fclose(fp);
        return 1;
    }

    /* ---- 3. Init test context ---- */

    test_ctx_t ctx = {0};
    ctx.h_com          = h_com;
    ctx.fp             = fp;
    ctx.file_path      = file_path;
    ctx.file_total_size = (uint32_t)fsize;
    ctx.block_size     = use_1k ? 1024 : 128;
    ctx.verbose        = verbose;
    ctx.current_file_index = 0;

    /* ---- 4. Create Ymodem sender ---- */

    static uint8_t  tx_buffer[YMODEM_STX_FRAME_LEN_BYTE];
    static ymodem_sender_t sender;

    if (!ymodem_sender_create(&sender, tx_buffer, sizeof(tx_buffer))) {
        fprintf(stderr, "[FATAL] Sender creation failed (buffer too small?)\n");
        serial_close(h_com);
        fclose(fp);
        return 1;
    }

    ymodem_sender_set_event_callback(&sender, on_ymodem_event, &ctx);
    ymodem_sender_set_send_packet_callback(&sender, on_send_packet, &ctx);

    if (use_1k) {
        ymodem_sender_enable_1k(&sender);
    }

    ymodem_sender_start(&sender);

    printf("[PROTO] Ymodem sender started (%s mode), waiting for receiver 'C'...\n",
        use_1k ? "1K" : "128B");
    printf("[PROTO] Please start Ymodem receive on the terminal side\n");

    /* ---- 5. Main loop ---- */

    uint8_t  rx_buf[256];
    ymodem_stage_e prev_stage = YMODEM_STAGE_IDLE;

    while (!ctx.transfer_done && !ctx.transfer_error) {

        int n = serial_read(h_com, rx_buf, sizeof(rx_buf));
        if (n > 0) {
            if (verbose) {
                printf("[RECV] %d bytes: ", n);
                for (int i = 0; i < n && i < 8; i++) {
                    printf("%02X ", rx_buf[i]);
                }
                if (n > 8) printf("...");
                printf("\n");
            }
            ymodem_sender_parse(&sender, rx_buf, (uint32_t)n);
        }
        else if (n < 0) {
            fprintf(stderr, "[ERROR] Serial read failed\n");
            ctx.transfer_error = true;
            break;
        }

        ymodem_sender_poll(&sender);

        if (sender.stage != prev_stage) {
            printf("[STAGE] %s -> %s\n",
                stage_to_str(prev_stage), stage_to_str(sender.stage));
            prev_stage = sender.stage;
        }

        if (sender.stage == YMODEM_STAGE_IDLE && prev_stage != YMODEM_STAGE_IDLE) {
            ctx.transfer_done = true;
        }

        if (n == 0) {
            Sleep(TEST_POLL_INTERVAL_MS);
        }
    }

    /* ---- 6. Cleanup ---- */

    printf("\n[RESULT] Transfer %s\n", ctx.transfer_error ? "FAILED" : "OK");
    printf("[RESULT] Sent: %u / %u bytes\n",
        sender.file_info.file_send_size, ctx.file_total_size);

    if (ctx.fp) {
        fclose(ctx.fp);
    }
    serial_close(h_com);

    return ctx.transfer_error ? 1 : 0;
}

/* ================================================================
 *  Standalone main (only when compiled independently)
 * ================================================================ */

#ifdef YMODEM_STANDALONE

uint32_t system_get_time_ms(void)
{
    return (uint32_t)GetTickCount64();
}

int main(int argc, char* argv[])
{
    const char* port_name = NULL;
    const char* file_path = NULL;
    bool use_1k    = false;
    bool verbose   = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--1k") == 0) {
            use_1k = true;
        }
        else if (strcmp(argv[i], "-v") == 0) {
            verbose = true;
        }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
        else if (port_name == NULL) {
            port_name = argv[i];
        }
        else if (file_path == NULL) {
            file_path = argv[i];
        }
    }

    if (port_name == NULL || file_path == NULL) {
        fprintf(stderr, "Error: required arguments missing\n\n");
        print_usage(argv[0]);
        return 1;
    }

    return ymodem_sender_test_run(port_name, file_path, use_1k, verbose);
}
#endif
