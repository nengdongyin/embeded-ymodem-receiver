/**
 * @file ymodem_receiver_test.c
 * @brief Windows Serial Ymodem Receiver Test Program
 *
 * This file is part of the ymodem test suite. It is NOT a standalone program.
 * Use the unified entry point in test.c to run receiver tests,
 * or define YMODEM_STANDALONE for a standalone build.
 *
 * Usage (via unified test.c):
 *   ymodem_test.exe -r <COM port> <save directory> [-v] [-h]
 *
 * Standalone build (MinGW/GCC):
 *   gcc -DYMODEM_STANDALONE -Isrc test/windows/ymodem_receiver_test.c src/ymodem_receiver.c src/ymodem_common.c -o ymodem_receiver_test.exe
 *
 * Standalone build (MSVC):
 *   cl /DYMODEM_STANDALONE /Isrc test/windows/ymodem_receiver_test.c src/ymodem_receiver.c src/ymodem_common.c /Fe:ymodem_receiver_test.exe
 *
 * Interactive test flow:
 *   1. Install virtual serial port software (com0com, VSPE, etc.)
 *   2. Create a virtual serial port pair, e.g. COM15 <-> COM16
 *   3. Run: ymodem_receiver_test.exe COM16 C:\received
 *   4. Use XShell/Tera Term to send file via Ymodem on COM15
 */

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../src/ymodem_receiver.h"

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
 *  Receiver test context
 * ================================================================ */

typedef struct {
    HANDLE      h_com;
    FILE*       fp;
    const char* save_path;
    bool        transfer_done;
    bool        transfer_error;
    bool        verbose;
    uint32_t    file_count;
} recv_test_ctx_t;

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
 *  File path builder: combine save_path + file_name
 * ================================================================ */

static bool build_file_path(const char* save_path, const char* file_name,
                            char* out, size_t out_size)
{
    size_t dir_len = strlen(save_path);
    size_t name_len = strlen(file_name);

    if (dir_len + name_len + 2 > out_size) {
        return false;
    }

    memcpy(out, save_path, dir_len);

    if (dir_len > 0 && save_path[dir_len - 1] != '\\') {
        out[dir_len] = '\\';
        dir_len++;
    }

    memcpy(out + dir_len, file_name, name_len + 1);
    return true;
}

/* ================================================================
 *  Ymodem callback functions
 * ================================================================ */

static void on_recv_event(ymodem_receiver_parser_t* parser,
    const ymodem_receiver_event_t* event, void* user_ctx)
{
    recv_test_ctx_t* ctx = (recv_test_ctx_t*)user_ctx;

    switch (event->type) {

    case YMODEM_RECV_EVENT_FILE_INFO: {
        char full_path[512];
        if (!build_file_path(ctx->save_path, event->file_name,
                             full_path, sizeof(full_path))) {
            fprintf(stderr, "[ERROR] File path too long\n");
            ctx->transfer_error = true;
            return;
        }

        ctx->fp = fopen(full_path, "wb");
        if (!ctx->fp) {
            fprintf(stderr, "[ERROR] Cannot create file: %s\n", full_path);
            ctx->transfer_error = true;
            return;
        }

        ctx->file_count++;
        printf("[EVENT] FILE_INFO  name=\"%s\" size=%u bytes  -> %s\n",
            event->file_name, event->file_size, full_path);
        break;
    }

    case YMODEM_RECV_EVENT_DATA_PACKET: {
        if (!ctx->fp) {
            fprintf(stderr, "[ERROR] DATA_PACKET but no file open\n");
            ctx->transfer_error = true;
            return;
        }

        size_t written = fwrite(event->data, 1, event->data_len, ctx->fp);
        if (written != event->data_len) {
            fprintf(stderr, "[ERROR] fwrite failed (wrote %zu of %u)\n",
                written, event->data_len);
            ctx->transfer_error = true;
            return;
        }

        if (ctx->verbose) {
            printf("[DATA] seq=%u len=%u total=%u\n",
                event->data_seq, event->data_len, event->total_received);
        }
        else {
            printf("\r[DATA] Progress: %u bytes", event->total_received);
            fflush(stdout);
        }
        break;
    }

    case YMODEM_RECV_EVENT_TRANSFER_COMPLETE:
        printf("\n[EVENT] TRANSFER_COMPLETE\n");
        if (ctx->fp) {
            fclose(ctx->fp);
            ctx->fp = NULL;
        }
        break;

    case YMODEM_RECV_EVENT_TRANSFER_FINISHED:
        printf("[EVENT] SESSION_FINISHED  total files: %u\n", ctx->file_count);
        ctx->transfer_done = true;
        break;

    case YMODEM_RECV_EVENT_ERROR:
        fprintf(stderr, "\n[EVENT] ERROR - Transfer aborted\n");
        ctx->transfer_error = true;
        break;

    default:
        break;
    }
}

static void on_send_response(ymodem_receiver_parser_t* parser, void* user_ctx)
{
    recv_test_ctx_t* ctx = (recv_test_ctx_t*)user_ctx;

    uint32_t len = parser->buffer.tx_buffer_ack_len;

    if (ctx->verbose) {
        printf("[RESP] %u bytes: ", len);
        for (uint32_t i = 0; i < len && i < 4; i++) {
            printf("%02X ", parser->buffer.tx_buffer[i]);
        }
        printf("\n");
    }

    if (!serial_write(ctx->h_com, parser->buffer.tx_buffer, len)) {
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
    printf("Ymodem Receiver Test Tool\n");
    printf("Usage: %s <COM port> <save directory> [options]\n\n", prog);
    printf("Arguments:\n");
    printf("  <COM port>        Serial port name, e.g. COM16\n");
    printf("  <save directory>  Directory to save received files\n\n");
    printf("Options:\n");
    printf("  -v                Verbose log mode\n");
    printf("  -h                Show this help\n\n");
    printf("Examples:\n");
    printf("  %s COM16 C:\\received\n", prog);
    printf("  %s COM16 C:\\received -v\n", prog);
    printf("\n");
    printf("Interactive test flow:\n");
    printf("  1. Install virtual serial port software (com0com, VSPE, etc.)\n");
    printf("  2. Create a virtual serial port pair, e.g. COM15 <-> COM16\n");
    printf("  3. Run: %s COM16 C:\\received\n", prog);
    printf("  4. Use XShell/Tera Term to send file via Ymodem on COM15\n");
}

/* ================================================================
 *  Receiver test run function (callable from unified test.c)
 * ================================================================ */

int ymodem_receiver_test_run(const char* port_name, const char* save_path,
                             bool verbose)
{
    /* ---- 1. Open serial port ---- */

    HANDLE h_com = serial_open(port_name);
    if (h_com == INVALID_HANDLE_VALUE) {
        return 1;
    }

    /* ---- 2. Init test context ---- */

    recv_test_ctx_t ctx = {0};
    ctx.h_com     = h_com;
    ctx.save_path = save_path;
    ctx.verbose   = verbose;

    /* ---- 3. Create Ymodem receiver ---- */

    static uint8_t  rx_buffer[YMODEM_STX_FRAME_LEN_BYTE];
    static ymodem_receiver_parser_t parser;

    if (!ymodem_receiver_create(&parser, rx_buffer, sizeof(rx_buffer))) {
        fprintf(stderr, "[FATAL] Receiver creation failed (buffer too small?)\n");
        serial_close(h_com);
        return 1;
    }

    ymodem_receiver_set_event_callback(&parser, on_recv_event, &ctx);
    ymodem_receiver_set_send_response_callback(&parser, on_send_response, &ctx);

    ymodem_receiver_start(&parser);

    printf("[PROTO] Ymodem receiver started, waiting for sender...\n");
    printf("[PROTO] Please start Ymodem send on the terminal side\n");

    /* ---- 4. Main loop ---- */

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
            ymodem_receiver_parse(&parser, rx_buf, (uint32_t)n);
        }
        else if (n < 0) {
            fprintf(stderr, "[ERROR] Serial read failed\n");
            ctx.transfer_error = true;
            break;
        }

        ymodem_receiver_poll(&parser);

        if (parser.stage != prev_stage) {
            printf("[STAGE] %s -> %s\n",
                stage_to_str(prev_stage), stage_to_str(parser.stage));
            prev_stage = parser.stage;
        }

        if (n == 0) {
            Sleep(TEST_POLL_INTERVAL_MS);
        }
    }

    /* ---- 5. Cleanup ---- */

    printf("\n[RESULT] Receive %s\n", ctx.transfer_error ? "FAILED" : "OK");
    printf("[RESULT] Received %u file(s)\n", ctx.file_count);

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
    const char* save_path = NULL;
    bool verbose = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
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
        else if (save_path == NULL) {
            save_path = argv[i];
        }
    }

    if (port_name == NULL || save_path == NULL) {
        fprintf(stderr, "Error: required arguments missing\n\n");
        print_usage(argv[0]);
        return 1;
    }

    return ymodem_receiver_test_run(port_name, save_path, verbose);
}
#endif
