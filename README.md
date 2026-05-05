# Ymodem 协议栈 — 用户手册

**项目名称**：Ymodem 嵌入式协议库
**版本**：v1.0
**文档日期**：2026-05
**适用范围**：裸机 MCU、RTOS（Zephyr / FreeRTOS / RT-Thread 等）、Windows/Linux 桌面测试

---

## 目录

1. [项目概述](#1-项目概述)
2. [设计原理与架构](#2-设计原理与架构)
3. [工程思想](#3-工程思想)
4. [Ymodem 协议基础](#4-ymodem-协议基础)
5. [API 参考手册](#5-api-参考手册)
6. [集成示例](#6-集成示例)
7. [构建与测试](#7-构建与测试)
8. [速度测试与性能分析](#8-速度测试与性能分析)
9. [已知限制与后续计划](#9-已知限制与后续计划)
10. [附录](#附录)

---

## 1. 项目概述

### 1.1 定位

这是一个**纯 C 语言、零依赖、跨平台**的 Ymodem 协议库，为嵌入式设备固件升级和文件传输场景设计。

核心目标：

- **src/ 目录可直接拷贝到任何平台编译**，不需要 #ifdef 平台判断
- **零 malloc / 零 free**，所有缓冲区由调用方静态分配
- **非阻塞解析**，每次调用逐字节处理，不占用 CPU 时间片
- **回调解耦 I/O**，协议层不感知物理传输介质（UART / SPI / BLE / 文件 / 网络均可）

### 1.2 目录结构

```
ymodem_receiver/
├── src/                                  # 核心协议库（跨平台、零依赖）
│   ├── ymodem_common.h  (143 行)         # 公共定义：帧结构宏、协议枚举、CRC API、系统时间接口
│   ├── ymodem_common.c  ( 71 行)         # CRC-16 CCITT 查表计算（256 项预计算表）
│   ├── ymodem_sender.h  (234 行)         # 发送端数据结构、事件枚举、API 声明
│   ├── ymodem_sender.c  (752 行)         # 发送端状态机 + 帧构建 + poll 超时
│   ├── ymodem_receiver.h(225 行)         # 接收端数据结构、事件枚举、API 声明
│   └── ymodem_receiver.c(826 行)         # 接收端逐字节解析器 + 延迟复位 + poll
│
├── test/
│   ├── unity/                            # Unity Test Framework 单元测试
│   │   ├── unity_core/                   #    框架源码（unity.h / unity.c）
│   │   ├── mocks/mock_time.c             #    时间 mock（测试可控时间）
│   │   └── unit/                         #    单元测试源文件
│   │       ├── test_common.c             #      CRC-16 测试（12 用例）
│   │       ├── test_sender.c             #      发送端测试（38 用例）
│   │       └── test_receiver.c           #      接收端测试（49 用例）
│   ├── windows/                          # Windows 集成测试套件
│   │   ├── test.c                        #    统一入口: -s 发送 / -r 接收
│   │   ├── ymodem_sender_test.c          #    发送端串口集成测试
│   │   └── ymodem_receiver_test.c        #    接收端串口集成测试
│   └── zephyr/                           # Zephyr RTOS 集成示例
│       └── zephyr_example.c              #    MCU 端收发完整示例（LittleFS + UART）
│
├── build_and_test.bat                    # Windows 全量自动化构建脚本
└── build_and_test.sh                     # Linux/macOS 全量自动化构建脚本
```

**核心库总代码量**：约 2251 行 C 代码，不依赖任何系统头文件（除 `<stdint.h>` 和 `<stdbool.h>`）。

**测试代码**：99 个单元测试全覆盖 CRC / 发送端 / 接收端核心路径。

---

## 2. 设计原理与架构

### 2.1 整体架构图

```
┌─────────────────────────────────────────────────────┐
│  应用层 (Application)                                │
│  - 文件 I/O 管理 (fopen / fread / fwrite)            │
│  - 传输状态显示                                       │
│  - 错误处理策略                                       │
├─────────────────────────────────────────────────────┤
│           ▲ 事件回调            ▼ 数据写入            │
│  ┌────────┴─────────┐  ┌────────┴─────────┐         │
│  │  ymodem_sender   │  │ ymodem_receiver  │         │
│  │  - 帧组装 (CRC)   │  │  - 帧解析 (校验)  │         │
│  │  - 状态机驱动     │  │  - ACK/NAK 应答   │         │
│  │  - 超时重传       │  │  - 填充字节截断   │         │
│  │  - file_index 递增│  │  - 延迟复位       │         │
│  └────────┬─────────┘  └────────┬─────────┘         │
│           │  发送回调              │  应答回调        │
├───────────┼───────────────────────┼─────────────────┤
│  传输层 (Transport)                │                  │
│  - UART 轮询/中断/DMA              │                  │
│  - 虚拟串口 / 物理串口             │                  │
└─────────────────────────────────────────────────────┘
```

库本身只有**两层**：

| 层级 | 职责 | 示例 |
|------|------|------|
| 协议层 (`src/`) | 帧组装/解析、CRC 校验、超时重传、状态机 | `ymodem_sender_parse()` |
| 应用层 (用户代码) | 文件 I/O、串口读写、`system_get_time_ms()` | 回调函数实现 |

没有中间层——应用层直接实现串口读写回调，协议层不感知物理层细节。

### 2.2 数据流设计（Sender 端）

```
ymodem_sender_start()
    │
    ▼
等待 'C' ──ymodem_sender_parse()◄── 串口收到 'C'
    │
    ▼
frame_build_with_data(EVENT_FILE_INFO)
    │
    ├── frame_user_event_callback() ──► 用户回调：填写 send->file_info
    │                                      (file_name, file_total_size)
    ├── ymodem_sender_file_info_pack()  组装文件名+大小帧
    ├── build_data_packet()             添加 SOH + seq + CRC
    └── sender_send_packet ──────────► 用户回调：uart_poll_out(...)
        (同步更新 last_time_ms)         ← 超时计时起点
    │
    ▼
等待 ACK ──ymodem_sender_parse()◄── 串口收到 ACK
    │   ymodem_sender_poll()     ← 超时→重发当前帧
    ▼
frame_build_with_data(EVENT_DATA_PACKET)
    │
    ├── 设置帧类型 (SOH 128B 或 STX 1024B)
    ├── frame_user_event_callback() ──► 用户回调：
    │     fread(event->data, 1024, fp)
    │     event->data_len = n        ◄── 写入实际数据长度
    │
    ├── active_len = event->data_len   ◄── 核心：读取用户填入的实际长度
    ├── build_data_packet()            添加填充 0x1A + CRC
    └── sender_send_packet ──────────► 串口发送整帧
    │
    ▼
循环...直到 file_send_size >= file_total_size
    │
    ▼
发送 EOT → 等 NAK → 发送第二个 EOT → 等 ACK
    │
    ▼
TRANSFER_COMPLETE, file_index++  → 回到 ESTABLISHING 等下一文件
    │  用户回调中 event->file_index 递增
    │  file_name[0]=='\0' 结束会话 → SESSION_FINISHED → IDLE
```

**关键设计**：

- `sender_send_packet()` 封装所有发包操作，**每次发包后自动刷新 `last_time_ms`**，确保 poll 超时从发包时刻起算
- `event->data_len` 由用户在回调中设置，协议层据此决定实际有效数据长度
- `event->file_index` 随每个文件传输完成自动递增，用户据此在 `FILE_INFO` 回调中判断是否还有下一个文件

### 2.3 数据流设计（Receiver 端）

Receiver 是一个**逐字节有限状态机**，对接收到的新帧头延迟复位：

```
IDLE
    │ ymodem_receiver_start() → 进入 ESTABLISHING（不立即发 'C'）
    ▼
ESTABLISHING ── poll 超时后自动发送 'C'，等待 SOH 帧 (seq=0)
    │
    │ 收到 SOH → 解析文件信息 → ACK+C → ESTABLISHED
    │   file_rev_frame_number → 1
    │
    ▼
ESTABLISHED ── 等待第一数据帧 (seq=1)
    │ 收到 → ACK → TRANSFERRING
    │   file_rev_size += data_len（带最后一包截断）
    │
    ▼
TRANSFERRING ── 持续接收数据帧
    │ 每次 ACK → 继续收下一帧
    │ 收到 EOT → NAK → FINISHING
    │
    ▼
FINISHING ── 等待第二个 EOT
    │ 收到 EOT → ACK+C → TRANSFER_COMPLETE → FINISHED
    │
    ▼
FINISHED ── 等待 SOH (seq=0)
    │ 文件名非空 → ESTABLISHED（新文件循环）
    │ 文件名为空 → TRANSFER_FINISHED → IDLE（会话结束）
    │
    ▼
IDLE

CANCEL 路径（任意阶段）：
  CAN → WAIT_CAN_2 → CAN → IDLE（触发 ERROR 事件）
```

**延迟复位机制**：`frame_stage_process()` 完成后仅设置 `frame_is_end=true`，真正的复位延迟到下一次解析遇到帧头字节（SOH/STX/EOT/CAN）时执行。帧间噪声字节（如填充 0x00）被跳过，用户可以在 `parse()` 返回后完整读取帧信息做测试断言。

### 2.4 发送端超时机制

```
每次 sender_send_packet() → send->process.last_time_ms = now

poll() 检查:
  if (now - last_time_ms >= YMODEM_TIMEOUT_MS):
    error = YMODEM_ERROR_TIME_OUT
    frame_stage_process() → frame_error_process()
      ├── error_count++
      ├── 未超限 → 重发 tx_buffer 中缓存帧（重传）
      └── 超限    → 发送 CAN-CAN → IDLE → 通知用户 ERROR
```

---

## 3. 工程思想

### 3.1 零动态内存分配

嵌入式系统应避免 `malloc`/`free` 导致的碎片化和不确定延迟。本库所有缓冲区均由调用方静态分配：

```c
// 调用方分配缓冲区（栈或静态区均可）
static uint8_t ymodem_buffer[YMODEM_STX_FRAME_LEN_BYTE]; // 至少 1029 字节
static ymodem_sender_t sender;

ymodem_sender_create(&sender, ymodem_buffer, sizeof(ymodem_buffer));
```

### 3.2 回调解耦 I/O

协议层不知道、也不需要知道你在用什么传输介质：

| 回调函数 | 职责 | 调用时机 |
|----------|------|----------|
| `send_event_callback` | 通知用户传输阶段事件 | 状态机每个阶段触发 |
| `send_packet` | 将组装好的帧写入物理通道 | 每帧就绪后 |
| `send_response` | 将 ACK/NAK/C/CAN 写入物理通道 | 接收端确认时 |

```c
// 同一个 src/ymodem_sender.c，可以在 Windows、Zephyr、裸机上运行：
// Windows: 回调里调 WriteFile(hCom, ...)
// Zephyr:  回调里调 uart_poll_out(uart_dev, ...)
// 裸机:    回调里调 USART_SendData(USART1, ...)
```

### 3.3 非阻塞设计

`ymodem_*_parse()` 不包含任何循环等待，每次只处理传入的字节数据：

```c
// 主循环（100Hz）
while (1) {
    int n = uart_read(rx_buf, sizeof(rx_buf));   // 非阻塞读
    if (n > 0)
        ymodem_receiver_parse(&parser, rx_buf, n);
    ymodem_receiver_poll(&parser);                // 超时检测
    sleep_ms(10);
}
```

调用方可以按任意频率调用 `parse` 和 `poll`，协议栈不会饿死其他任务。

### 3.4 平台时间接口

`system_get_time_ms()` 是库与应用层唯一的强依赖：

```c
// 用户必须实现（链接时缺失会报错）
uint32_t system_get_time_ms(void)
{
    return k_uptime_get_32();        // Zephyr
    // return HAL_GetTick();         // STM32 HAL
    // return GetTickCount64();      // Windows
    // return xTaskGetTickCount();   // FreeRTOS (需 scale 到 ms)
}
```

库不提供默认实现——如果用户忘记实现，链接器直接报错，避免静默返回 0 导致协议行为异常。

---

## 4. Ymodem 协议基础

### 4.1 帧格式

```
┌──────┬──────┬──────┬─────────────┬──────┬──────┐
│ SOH  │ seq  │ ~seq │   Data      │ CRC  │ CRC  │
│ 0x01 │ 0xNN │ 0xMM │ 128 bytes   │  hi  │  lo  │
│ 1 B  │ 1 B  │ 1 B  │   128 B     │ 1 B  │ 1 B  │
└──────┴──────┴──────┴─────────────┴──────┴──────┘
                        ├── CRC 计算范围 ──┤

SOH 帧总长 = 3 + 128 + 2 = 133 字节
STX 帧总长 = 3 + 1024 + 2 = 1029 字节
```

- `seq` 从 0 开始递增，`~seq` 为其按位取反（`seq + ~seq = 0xFF`）
- CRC 为 CCITT 多项式 `0x1021`，初值 0
- 最后一帧数据不足时，剩余空间填 `0x1A`（SUB 字符）

### 4.2 控制字符

| 字符 | 值 | 含义 |
|------|-----|------|
| SOH | 0x01 | 128 字节数据帧头 |
| STX | 0x02 | 1024 字节数据帧头 |
| EOT | 0x04 | 传输结束 |
| ACK | 0x06 | 确认 |
| NAK | 0x15 | 否定确认（请求重传） |
| CAN | 0x18 | 取消传输（连续两个 CAN） |
| C | 0x43 | CRC 模式握手请求 |

### 4.3 协议阶段（发送端视角）

```
IDLE
  │ start()
  ▼
ESTABLISHING    ← 等待接收方 'C'
  │ 收到 'C' → 发 FILE_INFO 帧
  ▼
ESTABLISHED     ← 文件信息帧已发送，等待 ACK+C
  │ 收到 ACK+C → 发首个数据帧 → TRANSFERRING
  │ (file_size==0 → EOT → FINISHING)
  ▼
TRANSFERRING    ← 持续发送数据帧
  │ file_send_size >= total → 发 EOT → FINISHING
  ▼
FINISHING       ← 第一个 EOT 已发送
  │ 发第二个 EOT → FINISHED
  ▼
FINISHED        ← TRANSFER_COMPLETE 回调, file_index++
  │ → ESTABLISHING（用户提供新文件 或 空文件名结束会话）
  ▼
ESTABLISHING / IDLE

CANCEL 路径（任意阶段）:
  CAN-CAN → IDLE（触发 ERROR 事件）
```

### 4.4 超时与重传

| 参数 | 值 | 说明 |
|------|-----|------|
| 超时时间 | 1000 ms | 可在 `ymodem_common.h` 中修改 `YMODEM_TIMEOUT_MS` |
| 最大重传次数 | 20 | `YMODEM_RETRANSMISSION_MAX_COUNT` |
| 超时行为 | 重发当前帧 | 适用于未收到 ACK/NAK 的情况 |
| 重传超限 | 发送 CAN-CAN，进入 IDLE | 防止死锁 |

### 4.5 多文件与会话结束

发送端通过 **`event->file_index` 自动递增**追踪文件序号。用户在 `FILE_INFO` 事件回调中判断：

- `event->file_index` 与本地文件列表中的某个位置匹配 → 填充 `file_name` 和 `file_total_size`
- `event->file_index` 超出已有文件范围 → 将 `file_name[0]` 设为 `'\0'`，发送器将自动发送空文件名包结束会话并触发 `SESSION_FINISHED` 事件

无需预声明"这是最后一个文件"——决策推迟到每个文件完成后、收到 `'C'` 请求下一个文件时才做出。

---

## 5. API 参考手册

### 5.1 发送端 API

#### 5.1.1 生命周期

```
ymodem_sender_create()        ← 必须最先调用，返回 true/false
ymodem_sender_set_event_callback()
ymodem_sender_set_send_packet_callback()
ymodem_sender_enable_1k()     ← 可选，启用 1K 模式
ymodem_sender_start()         ← 启动传输，返回 true/false
    ┌─────────────────┐
    │ ymodem_sender_parse()  ← 循环调用，喂入接收端响应字节；返回 ymodem_error_e
    │ ymodem_sender_poll()   ← 循环调用，超时检测；返回 true/false
    └─────────────────┘
```

**返回值说明**：
- `ymodem_sender_parse()` 返回 `YMODEM_ERROR_NONE`（成功处理响应）、`YMODEM_ERROR_GARBAGE`（非 Ymodem 数据）、`YMODEM_ERROR_WAIT_MORE`（等待更多字节）或其他错误码
- `ymodem_sender_poll()` 返回 `true`（触发了超时处理）、`false`（未超时或 IDLE）
- `ymodem_sender_start()` 返回 `true`（启动成功）、`false`（send 为 NULL）

#### 5.1.2 事件回调

```c
typedef void (*ymodem_sender_event_callback_t)(
    ymodem_sender_t* send,
    ymodem_sender_event_t* event,   // ← 非 const，可写 data_len
    void* user_ctx);
```

| 事件类型 | 含义 | 用户操作 |
|----------|------|----------|
| `YMODEM_SENDER_EVENT_FILE_INFO` | 需要文件信息 | 检查 `event->file_index`，填充 `file_name`、`file_total_size`；无可发文件则设 `file_name[0]='\0'` |
| `YMODEM_SENDER_EVENT_DATA_PACKET` | 需要数据块 | 写入 `event->data`、设置 `event->data_len` |
| `YMODEM_SENDER_EVENT_TRANSFER_COMPLETE` | 文件发送完成 | 关闭文件 |
| `YMODEM_SENDER_EVENT_SESSION_FINISHED` | 会话结束 | 清理资源 |
| `YMODEM_SENDER_EVENT_ERROR` | 发生错误 | 关闭文件，清理资源 |

**重要**：`event->file_index` 从 0 开始，每完成一个文件自动递增。用户在 `FILE_INFO` 中据此决定是否还有下一个文件。

#### 5.1.3 发送回调

```c
typedef void (*ymodem_sender_send_packet_t)(
    ymodem_sender_t* send,
    ymodem_sender_event_t* send_event,
    void* user_ctx);

// 实现示例：
void on_send_packet(ymodem_sender_t* send, ymodem_sender_event_t* evt, void* ctx)
{
    for (uint32_t i = 0; i < send->buffer.tx_buffer_active_len; i++)
        uart_poll_out(uart_dev, send->buffer.tx_buffer[i]);
}
```

### 5.2 接收端 API

#### 5.2.1 生命周期

```
ymodem_receiver_create()        ← 必须最先调用，返回 true/false
ymodem_receiver_set_event_callback()
ymodem_receiver_set_send_response_callback()
ymodem_receiver_start()         ← 启动，进入 ESTABLISHING（不立即发 'C'），返回 true/false
    ┌─────────────────┐
    │ ymodem_receiver_parse()  ← 循环调用，喂入接收字节；返回 ymodem_error_e
    │ ymodem_receiver_poll()   ← 循环调用，超时检测；返回 true/false
    └─────────────────┘
```

**返回值说明**：
- `ymodem_receiver_parse()` 返回 `YMODEM_ERROR_NONE`（完整处理一帧）、`YMODEM_ERROR_WAIT_MORE`（数据不足）、`YMODEM_ERROR_GARBAGE`（帧间非帧头字节）或其他错误码
- `ymodem_receiver_poll()` 返回 `true`（触发了超时处理，发送了 NAK 或 'C'）、`false`（未超时或 IDLE）
- `ymodem_receiver_start()` 返回 `true`（启动成功）、`false`（parser 为 NULL）

**延迟握手设计**：`start()` 仅进入 ESTABLISHING 阶段，不立即发送 'C'。真正的 'C' 由 `poll()` 超时后自动发出。这使得调用方可以在 `start()` 后完成其他协议的清理（如 AT 命令排空），再开始 Ymodem 握手。

#### 5.2.2 事件回调

```c
typedef void (*ymodem_receiver_event_callback_t)(
    ymodem_receiver_parser_t* parser,
    const ymodem_receiver_event_t* event,  // ← const，只读
    void* user_ctx);
```

| 事件类型 | 含义 | 用户操作 |
|----------|------|----------|
| `YMODEM_RECV_EVENT_FILE_INFO` | 收到文件信息 | `event->file_name` 含文件名，打开文件准备写入 |
| `YMODEM_RECV_EVENT_DATA_PACKET` | 收到数据块 | `event->data` 指向数据，`event->data_len` 为有效长度，写入文件 |
| `YMODEM_RECV_EVENT_TRANSFER_COMPLETE` | 文件接收完成 | 关闭文件 |
| `YMODEM_RECV_EVENT_TRANSFER_FINISHED` | 会话结束 | 协议栈回到 IDLE |
| `YMODEM_RECV_EVENT_ERROR` | 发生错误 | 关闭文件，清理资源 |

#### 5.2.3 应答回调

```c
typedef void (*ymodem_receiver_send_response_t)(
    ymodem_receiver_parser_t* parser,
    void* user_ctx);

// 实现示例：
void on_response(ymodem_receiver_parser_t* parser, void* ctx)
{
    for (uint32_t i = 0; i < parser->buffer.tx_buffer_ack_len; i++)
        uart_poll_out(uart_dev, parser->buffer.tx_buffer[i]);
}
```

### 5.3 公共接口 (`ymodem_common.h`)

| 函数/宏 | 说明 |
|----------|------|
| `ymodem_calculate_crc16(data, size)` | 计算 XMODEM CCITT CRC-16 |
| `system_get_time_ms()` | 用户必须实现的毫秒时间戳 |
| `YMODEM_TIMEOUT_MS` | 超时阈值（默认 1000ms） |
| `YMODEM_RETRANSMISSION_MAX_COUNT` | 最大重传次数（默认 20） |
| `YMODEM_SOH_FRAME_LEN_BYTE` | SOH 帧总长（133） |
| `YMODEM_STX_FRAME_LEN_BYTE` | STX 帧总长（1029） |

### 5.4 扩展 API

| 函数 | 说明 |
|------|------|
| `ymodem_sender_reset(send)` | 手动复位发送器帧级状态（按错误类型分级复位） |
| `ymodem_sender_enable_1k(send)` | 启用 1024 字节 STX 帧模式 |
| `ymodem_receiver_reset(parser)` | 手动复位接收器帧级状态（按错误类型分级复位） |

---

## 6. 集成示例

### 6.1 Windows 示例

```c
// 简化版 Windows Sender 核心代码
#include "ymodem_sender.h"
#include <windows.h>

HANDLE hCom;
FILE*  fp;
uint32_t file_count = 0;   // 已发送文件计数

uint32_t system_get_time_ms(void) {
    return (uint32_t)GetTickCount64();
}

// 事件回调：协议层通知你该做什么
void on_event(ymodem_sender_t* send, ymodem_sender_event_t* event, void* ctx) {
    switch (event->type) {
    case YMODEM_SENDER_EVENT_FILE_INFO:
        // file_index 从 0 开始，每完成一个文件自动递增
        if (event->file_index >= file_count) {
            // 没有更多文件 → 留空文件名 → 会话结束
            send->file_info.file_name[0] = '\0';
        } else {
            strncpy(send->file_info.file_name, "firmware.bin", 127);
            send->file_info.file_total_size = 1048576;
        }
        break;
    case YMODEM_SENDER_EVENT_DATA_PACKET:
        fread(event->data, 1, 1024, fp);
        event->data_len = /* fread 返回值 */;
        break;
    case YMODEM_SENDER_EVENT_TRANSFER_COMPLETE:
        fclose(fp);
        break;
    case YMODEM_SENDER_EVENT_ERROR:
        printf("Transfer failed!\n");
        break;
    }
}

// 发送回调：把帧数据写入串口
void on_send(ymodem_sender_t* send, ymodem_sender_event_t* evt, void* ctx) {
    DWORD written;
    WriteFile(hCom, send->buffer.tx_buffer,
              send->buffer.tx_buffer_active_len, &written, NULL);
}

int main() {
    uint8_t tx_buf[YMODEM_STX_FRAME_LEN_BYTE];
    ymodem_sender_t sender;

    ymodem_sender_create(&sender, tx_buf, sizeof(tx_buf));
    ymodem_sender_set_event_callback(&sender, on_event, NULL);
    ymodem_sender_set_send_packet_callback(&sender, on_send, NULL);
    ymodem_sender_enable_1k(&sender);  // 启用 1024 字节帧
    ymodem_sender_start(&sender);

    uint8_t rx[256];
    while (1) {
        DWORD n;
        ReadFile(hCom, rx, sizeof(rx), &n, NULL);
        if (n > 0) ymodem_sender_parse(&sender, rx, n);
        ymodem_sender_poll(&sender);
        if (sender.stage == YMODEM_STAGE_IDLE) break;
    }
}
```

### 6.2 Zephyr RTOS 示例（MCU 端完整代码）

#### 6.2.1 接收端回调

```c
typedef struct {
    struct fs_file_t file;
    bool   file_open;
    char  *save_dir;
    bool   transfer_done;
    uint32_t file_size;
    uint32_t total_received;
} recv_ctx_t;

void on_recv_event(ymodem_receiver_parser_t *parser,
                   const ymodem_receiver_event_t *event, void *ctx) {
    recv_ctx_t *c = (recv_ctx_t *)ctx;

    switch (event->type) {
    case YMODEM_RECV_EVENT_FILE_INFO: {
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", c->save_dir, event->file_name);
        fs_file_t_init(&c->file);
        int rc = fs_open(&c->file, path, FS_O_CREATE | FS_O_RDWR);
        c->file_open = (rc == 0);
        c->file_size = event->file_size;
        LOG_INF("receiver: FILE_INFO name=%s size=%u", event->file_name, event->file_size);
        break;
    }
    case YMODEM_RECV_EVENT_DATA_PACKET: {
        fs_write(&c->file, event->data, event->data_len);
        c->total_received = event->total_received;
        break;
    }
    case YMODEM_RECV_EVENT_TRANSFER_COMPLETE:
        fs_close(&c->file);
        c->file_open = false;
        break;
    case YMODEM_RECV_EVENT_TRANSFER_FINISHED:
        c->transfer_done = true;
        break;
    }
}

void on_recv_response(ymodem_receiver_parser_t *parser, void *ctx) {
    for (uint32_t i = 0; i < parser->buffer.tx_buffer_ack_len; i++)
        uart_poll_out(uart_dev, parser->buffer.tx_buffer[i]);
}
```

#### 6.2.2 发送端回调（file_index 驱动）

```c
typedef struct {
    struct fs_file_t file;
    bool   file_open;
    uint32_t file_size;
    uint32_t sent_bytes;
    const char *file_path;
    uint32_t current_file_index;   // 与 event->file_index 比较
} send_ctx_t;

void on_sender_event(ymodem_sender_t *send, ymodem_sender_event_t *event, void *ctx) {
    send_ctx_t *c = (send_ctx_t *)ctx;

    switch (event->type) {
    case YMODEM_SENDER_EVENT_FILE_INFO:
        if (event->file_index != c->current_file_index) {
            // 没有下一个文件 → 留空文件名结束会话
            if (c->file_open) { fs_close(&c->file); c->file_open = false; }
            send->file_info.file_name[0] = '\0';
            send->file_info.file_total_size = 0;
        } else {
            fs_open(&c->file, c->file_path, FS_O_READ);
            c->file_open = true;
            c->sent_bytes = 0;
            const char *name = strrchr(c->file_path, '/');
            name = name ? name + 1 : c->file_path;
            strncpy(send->file_info.file_name, name, sizeof(send->file_info.file_name) - 1);
            send->file_info.file_total_size = c->file_size;
        }
        break;
    case YMODEM_SENDER_EVENT_DATA_PACKET: {
        uint32_t offset = event->data_seq * 1024;
        uint32_t remaining = c->file_size - offset;
        uint32_t to_read = remaining < 1024 ? remaining : 1024;
        ssize_t n = fs_read(&c->file, event->data, to_read);
        event->data_len = (uint32_t)n;
        c->sent_bytes += n;
        break;
    }
    }
}
```

### 6.3 关键集成检查清单

- [ ] 实现 `system_get_time_ms()`，返回单调递增毫秒时间戳
- [ ] 分配发送/接收缓冲区，大小至少 `YMODEM_STX_FRAME_LEN_BYTE` (1029 字节)
- [ ] 实现串口发送回调（每次发 `tx_buffer_active_len` 字节）
- [ ] 实现串口读取并调用 `ymodem_*_parse()`
- [ ] 循环中调用 `ymodem_*_poll()` 处理超时
- [ ] 事件回调中处理 `FILE_INFO` / `DATA_PACKET` / `COMPLETE` / `ERROR`
- [ ] Sender 端 `event->data_len` 在 `DATA_PACKET` 中必须设置
- [ ] Sender 端 `FILE_INFO` 回调中按 `event->file_index` 判断是否还有文件
- [ ] Sender 端不再需要 `ymodem_sender_finish()` 预声明（已移除）

---

## 7. 构建与测试

### 7.1 构建脚本

```bash
# Windows
build_and_test.bat           # 运行全部 99 个单元测试
build_and_test.bat receiver  # 仅接收端测试 (49 用例)
build_and_test.bat sender    # 仅发送端测试 (38 用例)
build_and_test.bat common    # 仅 CRC 测试 (12 用例)

# Linux / macOS
./build_and_test.sh          # 运行全部测试
```

脚本自动检测 GCC 或 MSVC，无需手动配置编译环境。

### 7.2 测试覆盖

| 测试文件 | 用例数 | 覆盖范围 |
|----------|--------|----------|
| `test_common.c` | 12 | CRC-16 空数据/已知向量/大缓冲区/确定性/SOH+STX帧容量 |
| `test_sender.c` | 38 | 初始化/空指针防护/ESTABLISHING/ESTABLISHED/TRANSFERRING/FINISHING/FINISHED/CAN取消/超时重传/重传超限/1K模式/IDLE重启/多文件/file_index递增验证/parse返回值/poll返回值 |
| `test_receiver.c` | 49 | 初始化/空指针防护/噪声忽略/帧头检测/SEQ验证/CRC错误/重传检测/IDLE取消/逐字节+分块解析/数据事件字段/最后一包截断/文件信息边界/EOT路径/poll超时/握手超限/多文件/会话结束/延迟复位验证/状态保留/parse返回值/poll返回值/GARBAGE检测/延迟握手 |
| **总计** | **99** | |

### 7.3 手动构建（单元测试）

```bash
# 以 test_receiver 为例
gcc -Isrc -Itest/unity/unity_core \
    test/unity/unity_core/unity.c \
    test/unity/mocks/mock_time.c \
    test/unity/unit/test_receiver.c \
    src/ymodem_receiver.c \
    src/ymodem_common.c \
    -o test/unity/unit/test_receiver.exe
```

---

## 8. 速度测试与性能分析

### 8.1 测试环境

| 参数 | Windows | Zephyr RTOS |
|------|---------|-------------|
| 波特率 | 115200 bps | 460800 bps |
| 帧模式 | SOH (128B) / STX (1024B) | STX (1024B) |
| 测试文件 | 1 MB 随机二进制 | 1 MB 随机二进制 |
| 校验方式 | 二进制对比 | 二进制对比 + 哈希校验 |
| 存储介质 | NTFS SSD | LittleFS + SPI Flash |

### 8.2 实测数据

#### Zephyr RTOS (MCU, STX 1K 模式)

| 方向 | 操作 | 耗时 | 速率 | 效率 |
|------|------|------|------|------|
| **Sender** | Flash 读取 → UART 发送 | 30.2 s | **34.7 KB/s** | 75.4% |
| **Receiver** | UART 接收 → Flash 写入 | 45.2 s | **23.2 KB/s** | 50.4% |

理论最大速率 @ 460800 bps 8N1：约 **46 KB/s**

#### 每帧耗时分解 (1024 字节数据包)

| 阶段 | Sender | Receiver |
|------|--------|----------|
| 串口传输 1029 字节 | ~22 ms | ~22 ms |
| Flash 读 / 写 | ~5 ms | ~20 ms |
| 协议开销 (ACK 往返) | ~3 ms | ~3 ms |
| **单帧总计** | **~30 ms** | **~45 ms** |

#### Windows 串口测试

| 方向 | 帧模式 | 结果 |
|------|--------|------|
| Sender → Tera Term | SOH 128B | ✅ 1 MB 二进制一致 |
| Sender → Tera Term | STX 1K | ✅ 1 MB 二进制一致 |
| Tera Term → Receiver | 自动协商 | ✅ 文件完整 |
| 本地回环 (Sender → Receiver) | STX 1K | ✅ 帧级验证通过 |

### 8.3 性能优化方向

| 优化手段 | 预期提升 | 难度 |
|----------|----------|------|
| 启用 STX 1K 模式（默认 SOH 128B） | ~3% | 一行代码 |
| UART 发送由轮询改为中断/DMA | +10~15 KB/s | 中 |
| Flash 写入聚合（多页缓存，批量擦除） | +5~10 KB/s | 高 |
| 提高波特率至 921600 / 1M | +50~100% | 低（硬件支持即可） |

---

## 9. 已知限制与后续计划

### 9.1 已知限制

| 限制 | 说明 | 影响 |
|------|------|------|
| 轮询式 UART 发送 | MCU 上轮询逐字节发送，占用 CPU | 高速传输时有 CPU 开销 |
| 单文件最大 4 GB | `file_total_size` 为 `uint32_t` | 对嵌入式场景足够 |
| 不支持 Ymodem-G（流模式） | 无 ACK 等待的批量发送 | 低速链路不受影响 |

### 9.2 后续计划

| 优先级 | 目标 | 预期工作量 |
|--------|------|-----------|
| P1 | MCU 端 UART 发送迁移到 DMA/中断驱动 | 中等 |
| P2 | 增加 `ymodem_port.h` 多平台移植抽象层 | 低 |
| P2 | Doxygen HTML 格式 API 文档生成 | 低 |

---

## 附录

### 附录 A：宏定义速查

| 宏 | 值 | 说明 |
|----|-----|------|
| `YMODEM_HEAD_LEN_BYTE` | 3 | 帧头长度（frame_type + seq + ~seq） |
| `YMODEM_CRC_LEN_BYTE` | 2 | CRC 长度 |
| `YMODEM_SOH_DATA_LEN_BYTE` | 128 | SOH 数据区容量 |
| `YMODEM_STX_DATA_LEN_BYTE` | 1024 | STX 数据区容量 |
| `YMODEM_SOH_FRAME_LEN_BYTE` | 133 | SOH 帧总长（3+128+2） |
| `YMODEM_STX_FRAME_LEN_BYTE` | 1029 | STX 帧总长（3+1024+2） |
| `YMODEM_FRAME_TYPE_BYTE_INDEX` | 0 | 帧类型在缓冲区的偏移 |
| `YMODEM_SEQ_BYTE_INDEX` | 1 | 序号在缓冲区的偏移 |
| `YMODEM_NOR_SEQ_BYTE_INDEX` | 2 | 序号反码在缓冲区的偏移 |
| `YMODEM_DATA_BYTE_INDEX` | 3 | 数据区在缓冲区的偏移 |
| `YMODEM_TIMEOUT_MS` | 1000 | 超时阈值 (ms) |
| `YMODEM_RETRANSMISSION_MAX_COUNT` | 20 | 最大重传次数 |

### 附录 B：错误码速查

| 错误码 | 含义 |
|--------|------|
| `YMODEM_ERROR_NONE` | 无错误（帧处理完成） |
| `YMODEM_ERROR_WAIT_MORE` | 数据不足，需继续喂入更多字节（`parse()` 返回值） |
| `YMODEM_ERROR_GARBAGE` | 帧间收到非帧头字节，非 Ymodem 数据（`parse()` 返回值） |
| `YMODEM_ERROR_TIME_OUT` | 超时未收到响应（poll 触发） |
| `YMODEM_ERROR_CRC` | CRC 校验失败 |
| `YMODEM_ERROR_SEQ` | 序号不匹配或反码校验失败 |
| `YMODEM_ERROR_HANDSHAKE_NACK` | 握手阶段 NAK |
| `YMODEM_ERROR_RETRANSMISSION_COUNT_MAX` | 重传次数达到上限 |
| `YMODEM_ERROR_SENDER_NO_REV_ACK` | 发送端未收到 ACK（接收端收到重复帧） |
| `YMODEM_ERROR_RESEND` | 请求重传（收到 NAK） |
| `YMODEM_ERROR_CAN` | 传输被 CAN-CAN 取消 |

### 附录 C：接收端解析状态枚举

| 状态 | 含义 |
|------|------|
| `YMODEM_RECV_WAIT_HEAD` | 等待帧头字节 |
| `YMODEM_RECV_WAIT_SEQ` | 等待序号 + 反码字节 |
| `YMODEM_RECV_WAIT_DATA` | 等待数据区字节 |
| `YMODEM_RECV_WAIT_CRC` | 等待 CRC 校验码 |
| `YMODEM_RECV_WAIT_CAN_2` | 等待第二个 CAN 字节 |

### 附录 D：发送端响应处理状态枚举

| 状态 | 含义 |
|------|------|
| `YMODEM_SENDER_WAIT_ACK` | 等待接收方 ACK（或 C） |
| `YMODEM_SENDER_WAIT_C` | 等待接收方字符 'C' |
| `YMODEM_SENDER_WAIT_CAN_2` | 等待第二个 CAN 字节 |
