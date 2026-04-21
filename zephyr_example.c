/*
 * Copyright (c) 2012-2014 Wind River Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <string.h>
#include <zephyr/logging/log.h>
#include "ymodem_rev.h"
LOG_MODULE_REGISTER(my_app, LOG_LEVEL_INF);
/* 定义栈大小和优先级 */
#define STACKSIZE 10240
#define PRIORITY 7
/* 通过设备树节点标号（node-label）获取设备句柄 */
#define UART1_NODE DT_NODELABEL(uart1)
/* 获取设备结构体指针 */
const struct device *const uart1_dev = DEVICE_DT_GET(UART1_NODE);

/* 1. 定义栈内存区域 */
K_THREAD_STACK_DEFINE(threadA_stack, STACKSIZE);
K_THREAD_STACK_DEFINE(threadB_stack, STACKSIZE);

/* 2. 定义线程控制块 */
static struct k_thread threadA_data;
static struct k_thread threadB_data;
/* 定义队列和队列项 */
K_PIPE_DEFINE(my_pipe, 2048, 1);
uint32_t system_get_time_ms(void)
{
     return k_uptime_get_32();
}
/* 中断回调函数 */
static void uart1_irq_callback(const struct device *dev, void *user_data)
{
    //printk("uart1_irq_callback\n");
    /* 必须调用此函数来更新中断状态 */
    if (!uart_irq_update(dev)) {
        return;
    }

    /* 处理接收中断 */
    if (uart_irq_rx_ready(dev)) {
        uint8_t rx_buf[64];
        int bytes_read;
        /* 一次性读取FIFO中所有数据*/
        bytes_read = uart_fifo_read(dev, rx_buf, sizeof(rx_buf));
        k_pipe_write(&my_pipe, rx_buf, bytes_read,K_NO_WAIT);
    }

    /* 处理发送中断*/
    if (uart_irq_tx_ready(dev)) {
    /* 如果需要基于中断的发送，可在此处调用 uart_fifo_fill() */
    }
}
/* 3. 线程入口函数 A */
void ymodem0_event_handler(ymodem_protocol_parser_t *p, const ymodem_event_t *e, void *ctx) {
    switch (e->type) {
    case YMODEM_EVENT_FILE_INFO:
        // 打开文件 e->file_name，准备写入
        LOG_INF("file name:%s size:%d!\n",e->file_name,e->file_size); 
        break;
    case YMODEM_EVENT_DATA_PACKET:
        // 将 e->data 写入文件
        LOG_INF("data seq:%d size:%d!\n",e->data_seq,e->data_len); 
        break;
    case YMODEM_EVENT_TRANSFER_COMPLETE:
        // 数据接收完成，可关闭文件（但可能还有结束包）
        LOG_INF("file send end!\n"); 
        break;
    case YMODEM_EVENT_SESSION_FINISHED:
        // 整个会话结束，解析器已静默
        LOG_INF("close talk!!\n"); 
        break;
    case YMODEM_EVENT_ERROR:
        LOG_INF("error!!\n"); 
        // 传输失败，清理资源
        break;
    }
}
void ymodem0_response_process(ymodem_protocol_parser_t* parser, void* user_ctx)
{
    LOG_INF("ymodem0_response_process!\n"); 
    for(int i = 0; i<parser->buffer.tx_buffer_ack_len;i++ ){
        uart_poll_out(uart1_dev, parser->buffer.tx_buffer[i]);
    }
}
void threadA(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);
    uint8_t buffer[133];
    uint8_t ymodem_buffer[1280];
    int bytes_read;

    ymodem_protocol_parser_t ymodem0;
    protocol_parser_create(&ymodem0,ymodem_buffer,1280);
    ymodem_set_event_callback(&ymodem0,ymodem0_event_handler,NULL);
    protocol_parser_set_send_response_callback(&ymodem0,ymodem0_response_process,NULL);
    ymodem_protocol_start(&ymodem0);
    
    while (1) {
        // bytes_read 由返回值返回
        bytes_read = k_pipe_read(&my_pipe, buffer, sizeof(buffer), K_MSEC(100));
        if(bytes_read > 0){
            ymodem_protocol_parser(&ymodem0,buffer,bytes_read);
        }
        else{
            ymodem_protocol_process_poll(&ymodem0);        
        }
    }
}

/* 线程入口函数 B */
void threadB(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    while (1) {
        LOG_INF("Thread B: Running\n");
        /* 休眠2000ms，让出CPU */
        k_msleep(2000);
    }
}
int main(void)
{
    /* 1. 检查设备是否就绪 */
    if (!device_is_ready(uart1_dev)) {
        LOG_INF("UART1 device not ready!\n");
    }
    /* 2. 配置并启用中断 */
    uart_irq_callback_user_data_set(uart1_dev, uart1_irq_callback, NULL);
    uart_irq_rx_enable(uart1_dev);
	/* 4. 创建线程，暂不启动 */
    k_thread_create(&threadA_data, threadA_stack,
                    K_THREAD_STACK_SIZEOF(threadA_stack),
                    threadA, NULL, NULL, NULL,
                    PRIORITY, 0, K_FOREVER);
    k_thread_name_set(&threadA_data, "thread_a");

    k_thread_create(&threadB_data, threadB_stack,
                    K_THREAD_STACK_SIZEOF(threadB_stack),
                    threadB, NULL, NULL, NULL,
                    PRIORITY, 0, K_FOREVER);
    k_thread_name_set(&threadB_data, "thread_b");

    /* 5. 启动线程 */
    k_thread_start(&threadA_data);
    k_thread_start(&threadB_data);

   /* 主线程也可以做自己的事情，或者直接返回*/
    while (1) {
        //LOG_INF("Main Thread: Idle\n");
        k_msleep(5000);
    }
	return 0;
}
