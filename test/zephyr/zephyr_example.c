/*
 * Copyright (c) 2012-2014 Wind River Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file zephyr_example.c
 * @brief Zephyr RTOS Ymodem sender/receiver integration example
 *
 * Configurable via Kconfig or compile-time defines:
 *   CONFIG_YMODEM_MODE_SENDER=y       -> sender mode (default: receiver)
 *   CONFIG_YMODEM_SENDER_FILE_PATH    -> file path for sender (default: "/lfs/test.bin")
 *   CONFIG_YMODEM_RECV_SAVE_DIR       -> save directory for receiver (default: "/lfs")
 */

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <string.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include "ymodem/ymodem_receiver.h"
#include "ymodem/ymodem_sender.h"

LOG_MODULE_REGISTER(ymodem_app, LOG_LEVEL_INF);

/* ================================================================
 *  Compile-time configuration
 * ================================================================ */

#define TEST_BAUD_RATE        460800
#define STACK_SIZE            10240
#define THREAD_PRIORITY       7
#define PIPE_BUFFER_SIZE      2048
#define YMODEM_RX_BUFFER_SIZE 1280
#define YMODEM_TX_BUFFER_SIZE 1280
#define RESPONSE_BUF_SIZE     256
#define FILE_PATH_MAX          256
#define BLOCK_SIZE_1K          1024

/* Kconfig overrides */
#ifndef CONFIG_YMODEM_SENDER_FILE_PATH
#define CONFIG_YMODEM_SENDER_FILE_PATH "/lfs/test.bin"
#endif
#ifndef CONFIG_YMODEM_RECV_SAVE_DIR
#define CONFIG_YMODEM_RECV_SAVE_DIR     "/lfs"
#endif

/* Mode selection: define CONFIG_YMODEM_MODE_SENDER=y in Kconfig for sender */
#ifdef CONFIG_YMODEM_MODE_SENDER
#define MODE_IS_SENDER 1
#else
#define MODE_IS_SENDER 0
#endif

/* ================================================================
 *  Device
 * ================================================================ */

#define UART1_NODE DT_NODELABEL(uart1)
const struct device *const uart1_dev = DEVICE_DT_GET(UART1_NODE);

/* ================================================================
 *  Thread resources
 * ================================================================ */

K_THREAD_STACK_DEFINE(protocol_stack, STACK_SIZE);
static struct k_thread protocol_thread_data;

K_PIPE_DEFINE(uart_pipe, PIPE_BUFFER_SIZE, 1);

/* ================================================================
 *  Platform time
 * ================================================================ */

uint32_t system_get_time_ms(void)
{
    return k_uptime_get_32();
}

/* ================================================================
 *  File I/O wrapper (thread-safe, error-handled, logged)
 * ================================================================ */

typedef enum {
    FILE_IO_OK = 0,
    FILE_IO_ERR_OPEN,
    FILE_IO_ERR_READ,
    FILE_IO_ERR_WRITE,
    FILE_IO_ERR_CLOSE,
    FILE_IO_ERR_SEEK,
    FILE_IO_ERR_READ_SIZE,
} file_io_result_t;

static int file_io_open(struct fs_file_t *f, const char *path, int flags)
{
    fs_file_t_init(f);
    int rc = fs_open(f, path, flags);
    if (rc != 0) {
        LOG_ERR("fs_open(%s) failed: %d", path, rc);
        return FILE_IO_ERR_OPEN;
    }
    LOG_INF("file opened: %s", path);
    return FILE_IO_OK;
}

static int file_io_read(struct fs_file_t *f, uint8_t *buf, uint32_t len,
                        uint32_t *out_read)
{
    ssize_t n = fs_read(f, buf, len);
    if (n < 0) {
        LOG_ERR("fs_read(%u) failed: %d", len, (int)n);
        return FILE_IO_ERR_READ;
    }
    *out_read = (uint32_t)n;
    if (*out_read != len) {
        LOG_WRN("fs_read partial: %u/%u bytes", *out_read, len);
    }
    return FILE_IO_OK;
}

static int file_io_write(struct fs_file_t *f, const uint8_t *data, uint32_t len)
{
    ssize_t n = fs_write(f, data, len);
    if (n < 0) {
        LOG_ERR("fs_write(%u) failed: %d", len, (int)n);
        return FILE_IO_ERR_WRITE;
    }
    if ((uint32_t)n != len) {
        LOG_ERR("fs_write partial: %d/%u bytes", (int)n, len);
        return FILE_IO_ERR_WRITE;
    }
    return FILE_IO_OK;
}

static int file_io_close(struct fs_file_t *f)
{
    int rc = fs_close(f);
    if (rc != 0) {
        LOG_ERR("fs_close failed: %d", rc);
        return FILE_IO_ERR_CLOSE;
    }
    return FILE_IO_OK;
}

static int file_io_seek(struct fs_file_t *f, uint32_t offset)
{
    int rc = fs_seek(f, offset, FS_SEEK_SET);
    if (rc != 0) {
        LOG_ERR("fs_seek(%u) failed: %d", offset, rc);
        return FILE_IO_ERR_SEEK;
    }
    return FILE_IO_OK;
}

/* ================================================================
 *  Sender context and callbacks
 * ================================================================ */

#if MODE_IS_SENDER

typedef struct {
    struct fs_file_t file;
    bool             file_open;
    const char      *file_path;
    uint32_t         file_size;
    uint32_t         sent_bytes;
    uint32_t         last_log_ms;
    uint32_t         current_file_index;
} send_ctx_t;

static send_ctx_t send_ctx;

static void on_sender_event(ymodem_sender_t *send,
                            ymodem_sender_event_t *event,
                            void *user_ctx)
{
    send_ctx_t *ctx = (send_ctx_t *)user_ctx;
    int rc;

    switch (event->type) {

    case YMODEM_SENDER_EVENT_FILE_INFO: {
        if (event->file_index != ctx->current_file_index) {
            if (ctx->file_open) {
                file_io_close(&ctx->file);
                ctx->file_open = false;
            }
            send->file_info.file_name[0] = '\0';
            send->file_info.file_total_size = 0;
            LOG_INF("sender: FILE_INFO no more files, ending session");
        } else {
            if (ctx->file_open) {
                file_io_close(&ctx->file);
                ctx->file_open = false;
            }
            rc = file_io_open(&ctx->file, ctx->file_path, FS_O_READ);
            if (rc != FILE_IO_OK) {
                LOG_ERR("sender: cannot open source file, aborting");
                return;
            }
            ctx->file_open = true;
            ctx->sent_bytes = 0;

            const char *name = strrchr(ctx->file_path, '/');
            name = name ? name + 1 : ctx->file_path;
            strncpy(send->file_info.file_name, name,
                    sizeof(send->file_info.file_name) - 1);
            send->file_info.file_total_size = ctx->file_size;

            LOG_INF("sender: FILE_INFO idx=%u name=\"%s\" size=%u",
                    event->file_index, send->file_info.file_name, ctx->file_size);
        }
        break;
    }

    case YMODEM_SENDER_EVENT_DATA_PACKET: {
        if (!ctx->file_open) {
            LOG_ERR("sender: DATA_PACKET but file not open");
            return;
        }

        uint32_t offset = event->data_seq * BLOCK_SIZE_1K;
        uint32_t remaining = ctx->file_size - offset;
        uint32_t to_read = (remaining < BLOCK_SIZE_1K) ? remaining : BLOCK_SIZE_1K;

        uint32_t n = 0;
        rc = file_io_read(&ctx->file, event->data, to_read, &n);
        if (rc != FILE_IO_OK) {
            LOG_ERR("sender: read failed at offset %u", offset);
            return;
        }

        event->data_len = n;
        ctx->sent_bytes += n;

        uint32_t now = k_uptime_get_32();
        if (now - ctx->last_log_ms > 2000) {
            ctx->last_log_ms = now;
            LOG_INF("sender: progress %u/%u bytes (%u%%)",
                    ctx->sent_bytes, ctx->file_size,
                    (uint32_t)(100ULL * ctx->sent_bytes / ctx->file_size));
        } else {
            LOG_DBG("sender: seq=%u off=%u len=%u/%u sent=%u/%u",
                    event->data_seq, offset, n, to_read,
                    ctx->sent_bytes, ctx->file_size);
        }
        break;
    }

    case YMODEM_SENDER_EVENT_TRANSFER_COMPLETE:
        LOG_INF("sender: TRANSFER_COMPLETE (sent %u bytes)", ctx->sent_bytes);
        if (ctx->file_open) {
            file_io_close(&ctx->file);
            ctx->file_open = false;
        }
        break;

    case YMODEM_SENDER_EVENT_SESSION_FINISHED:
        LOG_INF("sender: SESSION_FINISHED");
        break;

    case YMODEM_SENDER_EVENT_ERROR:
        LOG_ERR("sender: transfer aborted (sent %u/%u bytes)",
                ctx->sent_bytes, ctx->file_size);
        if (ctx->file_open) {
            file_io_close(&ctx->file);
            ctx->file_open = false;
        }
        break;

    default:
        break;
    }
}

static void on_sender_send_packet(ymodem_sender_t *send,
                                  ymodem_sender_event_t *send_event,
                                  void *user_ctx)
{
    for (uint32_t i = 0; i < send->buffer.tx_buffer_active_len; i++) {
        uart_poll_out(uart1_dev, send->buffer.tx_buffer[i]);
    }
}

static bool get_file_size(const char *path, uint32_t *out_size)
{
    struct fs_dirent entry;
    int rc = fs_stat(path, &entry);
    if (rc != 0) {
        LOG_ERR("fs_stat(%s) failed: %d", path, rc);
        return false;
    }
    if (entry.type != FS_DIR_ENTRY_FILE) {
        LOG_ERR("%s is not a regular file", path);
        return false;
    }
    *out_size = (uint32_t)entry.size;
    return true;
}

#endif /* MODE_IS_SENDER */

/* ================================================================
 *  Receiver context and callbacks
 * ================================================================ */

#if !MODE_IS_SENDER

typedef struct {
    struct fs_file_t file;
    bool             file_open;
    const char      *save_dir;
    bool             transfer_done;
    uint32_t         file_size;
    uint32_t         total_received;
    uint32_t         last_log_ms;
} recv_ctx_t;

static recv_ctx_t recv_ctx;

static void on_receiver_event(ymodem_receiver_parser_t *parser,
                              const ymodem_receiver_event_t *event,
                              void *user_ctx)
{
    recv_ctx_t *ctx = (recv_ctx_t *)user_ctx;
    char full_path[FILE_PATH_MAX];
    int rc;

    switch (event->type) {

    case YMODEM_RECV_EVENT_FILE_INFO: {
        if (snprintf(full_path, sizeof(full_path), "%s/%s",
                     ctx->save_dir, event->file_name) >= (int)sizeof(full_path)) {
            LOG_ERR("receiver: file path too long");
            return;
        }

        rc = file_io_open(&ctx->file, full_path,
                          FS_O_CREATE | FS_O_RDWR);
        if (rc != FILE_IO_OK) {
            LOG_ERR("receiver: cannot create %s", full_path);
            return;
        }
        ctx->file_open = true;
        ctx->file_size = event->file_size;

        LOG_INF("receiver: FILE_INFO name=\"%s\" size=%u -> %s",
                event->file_name, event->file_size, full_path);
        break;
    }

    case YMODEM_RECV_EVENT_DATA_PACKET: {
        if (!ctx->file_open) {
            LOG_ERR("receiver: DATA_PACKET but no file open");
            return;
        }

        rc = file_io_write(&ctx->file, event->data, event->data_len);
        if (rc != FILE_IO_OK) {
            LOG_ERR("receiver: write failed at seq=%u", event->data_seq);
            file_io_close(&ctx->file);
            ctx->file_open = false;
            return;
        }

        ctx->total_received = event->total_received;

        LOG_DBG("receiver: seq=%u len=%u total=%u",
                event->data_seq, event->data_len, event->total_received);

        uint32_t now = k_uptime_get_32();
        if (ctx->file_size > 0 && now - ctx->last_log_ms > 2000) {
            ctx->last_log_ms = now;
            LOG_INF("receiver: progress %u/%u bytes (%u%%)",
                    event->total_received, ctx->file_size,
                    (uint32_t)(100ULL * event->total_received / ctx->file_size));
        }
        break;
    }

    case YMODEM_RECV_EVENT_TRANSFER_COMPLETE:
        LOG_INF("receiver: TRANSFER_COMPLETE");
        if (ctx->file_open) {
            file_io_close(&ctx->file);
            ctx->file_open = false;
        }
        break;

    case YMODEM_RECV_EVENT_TRANSFER_FINISHED:
        LOG_INF("receiver: session finished");
        ctx->transfer_done = true;
        break;

    case YMODEM_RECV_EVENT_ERROR:
        LOG_ERR("receiver: transfer aborted");
        if (ctx->file_open) {
            file_io_close(&ctx->file);
            ctx->file_open = false;
        }
        break;

    default:
        break;
    }
}

static void on_receiver_send_response(ymodem_receiver_parser_t *parser,
                                      void *user_ctx)
{
    for (uint32_t i = 0; i < parser->buffer.tx_buffer_ack_len; i++) {
        uart_poll_out(uart1_dev, parser->buffer.tx_buffer[i]);
    }
}

#endif /* !MODE_IS_SENDER */

/* ================================================================
 *  UART interrupt handler
 * ================================================================ */

static void uart1_irq_callback(const struct device *dev, void *user_data)
{
    if (!uart_irq_update(dev)) {
        return;
    }

    if (uart_irq_rx_ready(dev)) {
        uint8_t rx_buf[YMODEM_STX_FRAME_LEN_BYTE];
        int bytes_read = uart_fifo_read(dev, rx_buf, sizeof(rx_buf));
        if (bytes_read > 0) {
            k_pipe_write(&uart_pipe, rx_buf, (size_t)bytes_read, K_NO_WAIT);
        }
    }

    if (uart_irq_tx_ready(dev)) {
        /* reserved for interrupt-driven TX */
    }
}

/* ================================================================
 *  UART configuration
 * ================================================================ */

static void set_baud_rate(const struct device *uart_dev, uint32_t new_baud)
{
    struct uart_config cfg;
    int ret;

    ret = uart_config_get(uart_dev, &cfg);
    if (ret != 0) {
        printk("Error: uart_config_get() failed (%d)\n", ret);
        return;
    }
    cfg.baudrate = new_baud;

    ret = uart_configure(uart_dev, &cfg);
    if (ret == 0) {
        printk("UART baudrate set to %u\n", new_baud);
    } else {
        printk("Error: uart_configure() failed (%d)\n", ret);
    }
}

/* ================================================================
 *  Protocol thread
 *
 *  If MODE_IS_SENDER:
 *    - Reads source file, sends to remote receiver via Ymodem
 *  If receiver mode:
 *    - Listens for incoming Ymodem transfer, saves to save_dir
 * ================================================================ */

void protocol_thread_entry(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

#if MODE_IS_SENDER

    /* ---- sender mode ---- */

    if (!get_file_size(CONFIG_YMODEM_SENDER_FILE_PATH, &send_ctx.file_size)) {
        LOG_ERR("sender: cannot access %s", CONFIG_YMODEM_SENDER_FILE_PATH);
        return;
    }

    send_ctx.file_path = CONFIG_YMODEM_SENDER_FILE_PATH;
    send_ctx.file_open = false;
    send_ctx.current_file_index = 0;

    static uint8_t ymodem_tx_buffer[YMODEM_TX_BUFFER_SIZE];
    static ymodem_sender_t sender;

    if (!ymodem_sender_create(&sender, ymodem_tx_buffer, sizeof(ymodem_tx_buffer))) {
        LOG_ERR("sender: ymodem_sender_create failed");
        return;
    }

    ymodem_sender_set_event_callback(&sender, on_sender_event, &send_ctx);
    ymodem_sender_set_send_packet_callback(&sender, on_sender_send_packet, NULL);
    ymodem_sender_enable_1k(&sender);
    ymodem_sender_start(&sender);

    LOG_INF("sender: started, waiting for receiver 'C' (%s, %u bytes)",
            CONFIG_YMODEM_SENDER_FILE_PATH, send_ctx.file_size);

    uint8_t response_buf[RESPONSE_BUF_SIZE];
    ymodem_stage_e prev_stage = YMODEM_STAGE_IDLE;
    uint32_t transfer_start_ms = 0;

    while (1) {
        int n = k_pipe_read(&uart_pipe, response_buf,
                            1, K_MSEC(100));
        if (n > 0) {
            ymodem_sender_parse(&sender, response_buf, (uint32_t)n);
        } else {
            ymodem_sender_poll(&sender);
        }

        if (sender.stage != prev_stage) {
            prev_stage = sender.stage;
            if (sender.stage == YMODEM_STAGE_TRANSFERRING) {
                transfer_start_ms = k_uptime_get_32();
            }
        }

        if (sender.stage == YMODEM_STAGE_IDLE) {
            uint32_t elapsed = transfer_start_ms > 0
                ? (k_uptime_get_32() - transfer_start_ms) : 0;
            LOG_INF("sender: done in %u ms (%u bytes, %u B/s)",
                    elapsed, send_ctx.sent_bytes,
                    elapsed ? (send_ctx.sent_bytes * 1000 / elapsed) : 0);
            break;
        }
    }

#else

    /* ---- receiver mode ---- */

    recv_ctx.save_dir = CONFIG_YMODEM_RECV_SAVE_DIR;
    recv_ctx.file_open = false;
    recv_ctx.transfer_done = false;

    static uint8_t ymodem_rx_buffer[YMODEM_RX_BUFFER_SIZE];
    static ymodem_receiver_parser_t parser;

    if (!ymodem_receiver_create(&parser, ymodem_rx_buffer, sizeof(ymodem_rx_buffer))) {
        LOG_ERR("receiver: ymodem_receiver_create failed");
        return;
    }

    ymodem_receiver_set_event_callback(&parser, on_receiver_event, &recv_ctx);
    ymodem_receiver_set_send_response_callback(&parser, on_receiver_send_response, NULL);
    ymodem_receiver_start(&parser);

    LOG_INF("receiver: started, waiting for sender...");

    uint8_t data_buf[YMODEM_STX_FRAME_LEN_BYTE];
    ymodem_stage_e prev_stage = YMODEM_STAGE_IDLE;
    uint32_t transfer_start_ms = 0;

    while (!recv_ctx.transfer_done) {
        int n = k_pipe_read(&uart_pipe, data_buf,
                            sizeof(data_buf), K_MSEC(100));
        if (n > 0) {
            ymodem_receiver_parse(&parser, data_buf, (uint32_t)n);
        } else {
            ymodem_receiver_poll(&parser);
        }

        if (parser.stage != prev_stage) {
            prev_stage = parser.stage;
            if (parser.stage == YMODEM_STAGE_TRANSFERRING) {
                transfer_start_ms = k_uptime_get_32();
            }
        }
    }

    uint32_t elapsed = transfer_start_ms > 0
        ? (k_uptime_get_32() - transfer_start_ms) : 0;
    LOG_INF("receiver: done in %u ms (%u bytes, %u B/s)",
            elapsed, recv_ctx.total_received,
            elapsed ? (recv_ctx.total_received * 1000 / elapsed) : 0);

#endif
}

/* ================================================================
 *  Main
 * ================================================================ */

int main(void)
{
    printk("=== Ymodem Zephyr Example (%s mode) ===\n",
           MODE_IS_SENDER ? "SENDER" : "RECEIVER");

    if (!device_is_ready(uart1_dev)) {
        printk("Error: UART1 device not ready\n");
        return -1;
    }

    set_baud_rate(uart1_dev, TEST_BAUD_RATE);

    uart_irq_callback_user_data_set(uart1_dev, uart1_irq_callback, NULL);
    uart_irq_rx_enable(uart1_dev);

    k_thread_create(&protocol_thread_data, protocol_stack,
                    K_THREAD_STACK_SIZEOF(protocol_stack),
                    protocol_thread_entry, NULL, NULL, NULL,
                    THREAD_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&protocol_thread_data, "ymodem");

    while (1) {
        k_msleep(5000);
    }
    return 0;
}
