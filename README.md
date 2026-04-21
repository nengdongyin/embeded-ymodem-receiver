<div align="center">

# Ymodem 接收端 · 嵌入式轻量级实现

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-embedded-brightgreen)]()
[![Language](https://img.shields.io/badge/language-C99-orange)]()

</div>

> 一个**轻量级、非阻塞、高可移植**的 Ymodem 协议接收端实现，专为资源受限的嵌入式设备（Bootloader、固件升级）设计。纯 C99 编写，零外部依赖，仅需提供毫秒时间戳与串口收发回调即可集成。

---

## ✨ 特性

| 特性 | 说明 |
|------|------|
| 🚀 **完全非阻塞** | 事件驱动状态机，不占用 CPU 等待，完美适配 RTOS 或裸机主循环。 |
| 📦 **轻量可移植** | 核心代码约 800 行，无平台依赖。 |
| 🔒 **工业级鲁棒性** | 支持 SOH (128 字节) 和 STX (1024 字节) 帧，自动适配；CRC16 校验（XMODEM 标准），查表法高效计算；完整实现双 EOT 结束流程、双 CAN 取消序列；超时重传、错误计数、自动恢复，重试超限后优雅终止。 |
| 🧩 **事件驱动架构** | 通过回调通知文件信息、数据包、传输完成、会话结束等关键事件，用户无需关心协议细节。 |
| 🧵 **多实例支持** | 所有状态封装在句柄中，可同时运行多个解析器实例。 |
| 🎛️ **灵活的内存管理** | 接收缓冲区由用户外部注入，零动态内存分配，RAM 占用完全可控。 |

---

## 🔧 快速开始

### 移植要求

您只需实现以下三项：

| 项目 | 说明 |
|------|------|
| `system_get_time_ms` | 返回系统上电后的毫秒时间戳（弱函数，需用户覆盖）。 |
| 串口发送回调 | 将 `tx_buffer` 中的数据通过物理串口发送。 |
| 串口接收数据喂入 | 将从串口收到的字节流周期性调用 `ymodem_protocol_parser` 喂入。 |

---

### 1️⃣ 实现时间戳函数

以 Zephyr 为例：

```c
uint32_t system_get_time_ms(void) {
    return k_uptime_get_32();
}
```

---

### 2️⃣ 初始化与启动

```c
ymodem_protocol_parser_t ymodem;
uint8_t rx_buffer[1030];  // 至少 1029 字节

// 创建解析器（初始处于 IDLE 静默状态）
protocol_parser_create(&ymodem, rx_buffer, sizeof(rx_buffer));

// 设置事件回调（处理文件信息、数据、结束等）
ymodem_set_event_callback(&ymodem, my_event_handler, NULL);

// 设置发送回调
protocol_parser_set_send_response_callback(&ymodem, my_send_response, NULL);

// 当需要开始传输时（例如收到激活命令 "rb -E\r"）
ymodem_protocol_start(&ymodem);  // 发送第一个 'C'，开始握手
```

---

### 3️⃣ 主循环集成

> **重要**：您必须在主循环或 RTOS 任务中周期性调用 `ymodem_protocol_process_poll` 来处理超时逻辑，建议周期为 **10~50 毫秒**。该函数内部会检查握手超时与数据帧超时，并自动触发重传或终止。

```c
while (1) {
    uint8_t buf[128];
    size_t len = uart_read_nonblock(buf, sizeof(buf));
    if (len > 0) {
        ymodem_protocol_parser(&ymodem, buf, len);
    }
    // 必须周期性调用！处理超时重传与握手维持
    ymodem_protocol_process_poll(&ymodem);

    k_msleep(10);  // 或根据系统 Tick 调整
}
```

> ⚠️ **注意**：若未调用 `ymodem_protocol_process_poll`，超时机制将完全失效，传输可能卡死。

---

## 📡 事件回调说明

通过 `ymodem_set_event_callback` 注册的回调会接收到以下事件：

| 事件类型 | 含义 | 用户典型操作 |
|----------|------|-------------|
| `YMODEM_EVENT_FILE_INFO` | 收到文件信息包（文件名、大小） | 打开文件，准备写入 |
| `YMODEM_EVENT_DATA_PACKET` | 收到一个数据包（负载数据指针、长度、序号、累计接收字节数） | 将数据写入文件或 Flash |
| `YMODEM_EVENT_TRANSFER_COMPLETE` | 所有数据包接收完毕（第二个 EOT 已应答） | 可选，用于更新进度或预关闭文件 |
| `YMODEM_EVENT_SESSION_FINISHED` | 会话正式结束（空文件名包已确认），解析器回到 IDLE 静默状态 | 关闭文件，清理资源，等待下一次激活 |
| `YMODEM_EVENT_ERROR` | 传输被取消（CAN）或重试超限，会话中止 | 关闭文件，清理资源，回到静默状态 |

### 回调示例

```c
void my_event_handler(ymodem_protocol_parser_t *parser,
                      const ymodem_event_t *evt, void *ctx) {
    switch (evt->type) {
    case YMODEM_EVENT_FILE_INFO:
        open_file(evt->file_name, evt->file_size);
        break;
    case YMODEM_EVENT_DATA_PACKET:
        write_data(evt->data, evt->data_len);
        break;
    case YMODEM_EVENT_SESSION_FINISHED:
    case YMODEM_EVENT_ERROR:
        close_file();
        break;
    default:
        break;
    }
}
```

---

## ⚙️ 配置宏

可在编译时定义以下宏调整行为（默认值已适用多数场景）：

| 宏名称 | 默认值 | 说明 |
|--------|--------|------|
| `YMODEM_RETRANSMISSION_MAX_COUNT` | `20` | 最大重传次数 |
| `YMODEM_TIMEOUT_MS` | `1000` | 帧超时/握手间隔（毫秒） |

---

## 🧪 测试与兼容性

✅ 与 **Xshell** 终端测试通过。

---

## 📄 许可证

本项目采用 **MIT 许可证**。详情见 LICENSE 文件。
