# Dual CAN ISO-TP 可移植协议栈 Demo

## 1. 项目是什么

这个工程是一个面向后续多平台复用的通信栈 Demo。

当前目标是把基于：

- `CAN 2.0`
- `ISO-15765 / ISO-TP`
- 裁判系统通讯协议

拆成清晰分层，让后续 `STM32XXXX`、裸机/RTOS，都只需要改很少的平台适配代码就能复用。

当前工程位置：

- [main.c](/E:/STM32_PROJ/F407yuntai/F407_LOOP_ISOTP_TEST/F407_LOOP_ISOTP_TEST/Core/Src/main.c:1)
- [通信协议v2.md](/E:/STM32_PROJ/F407yuntai/F407_LOOP_ISOTP_TEST/F407_LOOP_ISOTP_TEST/通信协议v2.md:1)

注意：项目重点是底层和ISOTP层，应用层并不完善。

## 2. 当前 Demo 做了什么

当前 `F407` Demo 是真实双 CAN 总线测试，不使用软件桥接，不使用假回环。

- `CAN1 = 500 kbps`
- `CAN2 = 500 kbps`
- `USART1 = 921600 baud`

主循环默认行为：

- `CAN1` 每 `1000 ms` 发送一帧较长的 `MSG_LAUNCHER_CTRL_CMD`
- `CAN2` 每 `200 ms` 发送一帧较短的 `MSG_STATUS_REPORT`
- `CAN2` 收到 `MSG_LAUNCHER_CTRL_CMD` 后自动回复 `MSG_ACK`
- 每 `1000 ms` 打印一次通信健康状态

串口输出日志会标明：

- 是 `CAN1` 还是 `CAN2`
- `TX / RX`
- ISO-TP 完整重组结果
- 应用层分发结果
- 调度器事件
- `TEC / REC / BOFF / EPVF / EWGF / LEC / HAL Error / ESR / TSR / RF0R`

## 3. 分层结构

### 3.1 平台无关层

- `transport/isotp`
  - ISO-TP 分帧、流控、重组、超时、发送完成回调
- `protocol/app_frame`
  - 应用层固定 8 字节头的编解码、边界检查、ACK 判定
- `protocol/tlv`
  - TLV 写入、遍历、按类型查找、U8/U16/U32 编解码辅助
- `protocol/app_ack`
  - ACK 帧构造、ACK 解析、ACK 与原请求匹配
- `service/dispatcher`
  - 按 `func_code` 分发已解码应用层帧
- `service/retry_ack_scheduler`
  - 优先级调度、自动分配序号、ACK 等待、超时重发、取消/复位/事件回调

### 3.2 平台适配层

- [app_can.c](/E:/STM32_PROJ/F407yuntai/F407_LOOP_ISOTP_TEST/F407_LOOP_ISOTP_TEST/Core/Src/app_can.c:1)
  - 硬件 CAN 初始化、滤波器、发送、接收中断桥接、错误诊断
- [app_log.c](/E:/STM32_PROJ/F407yuntai/F407_LOOP_ISOTP_TEST/F407_LOOP_ISOTP_TEST/Core/Src/app_log.c:1)
  - 日志输出适配

移植时原则上优先只改这两层。

## 4. 第一版当前实际支持范围

这套协议栈不是把 `通信协议v2` 里所有预留消息都一次性做完，而是按“当前第一版真正要落地的消息”收口。

当前优先支持的功能码主要是：

- `MSG_HEARTBEAT`
- `MSG_STATUS_REPORT`
- `MSG_INIT_STATE`
- `MSG_CALIBRATION_STATE`
- `MSG_ERROR_REPORT`
- `MSG_ACK`
- `MSG_POWER_STATE`
- `MSG_HEAT_STATE`
- `MSG_BARREL_STATE`
- `MSG_SHOOT_EVENT`
- `MSG_POWER_LIMIT_CMD`
- `MSG_HEAT_LIMIT_CMD`
- `MSG_SHOOT_ENABLE_CMD`
- `MSG_LAUNCHER_CTRL_CMD`

当前优先保留的 TLV 类型主要是：

- `TLV_LEVEL`
- `TLV_SOURCE`
- `TLV_STATE`
- `TLV_RECOVERABLE`
- `TLV_VALUE`
- `TLV_TEXT`
- `TLV_REASON`
- `TLV_HP_CURRENT`
- `TLV_HP_MAX`
- `TLV_POWER_CURRENT`
- `TLV_POWER_LIMIT`
- `TLV_BUFFER_ENERGY`
- `TLV_HEAT_CURRENT`
- `TLV_HEAT_LIMIT`
- `TLV_COOLING_RATE`
- `TLV_SHOOT_ENABLE`
- `TLV_BULLET_TYPE`
- `TLV_SHOT_SEQ`
- `TLV_PROJECTILE_SPEED`
- `TLV_RESULT`
- `TLV_CMD_INDEX`
- `TLV_PAYLOAD`
- `TLV_ACK_FUNC`

### 4.1 当前刻意不提前暴露的内容

下面这些在 `通信协议v2` 里虽然出现过，但目前属于预留消息、暂缓实现消息，或者第一版业务还没有真正依赖，因此当前不作为主推 API 暴露：

- `TLV_POS_X / TLV_POS_Y / TLV_ANGLE`
- `TLV_ENABLE`
- `TLV_VAR_ID`
- `TLV_BULLET_17_REMAIN / TLV_BULLET_42_REMAIN`
- 帧级 `float`
- 帧级 `int16_t`

设计原则是：

- 先把第一版真正要跑的枪管链路、ACK 链路、状态链路做稳
- 不把还没落地的预留能力提前做进常用 API
- 等对应消息真的要上线时，再补最小必要接口

## 5. 为什么这个栈可以直接承载应用层协议

这个协议本质是：

- 
  
  固定应用层帧头
- 后接可变长度数据区
- 复杂消息优先使用 `TLV`

ISO-TP 负责解决 CAN 单帧最多 8 字节的问题，所以应用层开发者不用再关心：

- 首帧/连续帧/流控帧
- 分包重组
- 帧序号
- 底层超时

应用层只需要面向完整消息：

- 构造 通信协议v2.md 里面的完整、应用层的帧
- 组织 TLV
- 指定优先级
- 指定是否需要 ACK

## 6. 应用层常用接口

### 6.1 构造应用层帧

- `Protocol_AppFrame_Init(...)`
- `Protocol_AppFrame_SetData(...)`
- `Protocol_AppFrame_SetU8(...)`
- `Protocol_AppFrame_SetU16(...)`
- `Protocol_AppFrame_SetU32(...)`
- `Protocol_AppFrame_SetString(...)`
- `Protocol_AppFrame_GetU8(...)`
- `Protocol_AppFrame_GetU16(...)`
- `Protocol_AppFrame_GetU32(...)`
- `Protocol_AppFrame_Encode(...)`
- `Protocol_AppFrame_Decode(...)`
- `Protocol_AppFrame_RequiresAck(...)`

说明：

- 当前帧级数据类型只保留第一版真正已使用的 `U8 / U16 / U32 / STRING / BYTES / TLV_STREAM`
- 当前没有把帧级 `float`、帧级 `int16_t` 作为常用 API 暴露

### 6.2 组织 TLV

- `Protocol_TlvWriter_Init(...)`
- `Protocol_TlvWriter_AppendBytes(...)`
- `Protocol_TlvWriter_AppendU8(...)`
- `Protocol_TlvWriter_AppendU16(...)`
- `Protocol_TlvWriter_AppendU32(...)`
- `Protocol_TlvWriter_AppendString(...)`
- `Protocol_TlvWriter_AppendBool(...)`
- `Protocol_Tlv_Find(...)`
- `Protocol_Tlv_ReadU8(...)`
- `Protocol_Tlv_ReadU16(...)`
- `Protocol_Tlv_ReadU32(...)`
- `Protocol_Tlv_ReadBool(...)`
- `Protocol_Tlv_ReadStringView(...)`

说明：

- 当前 TLV API 暂时只覆盖·`通讯协议v2`已落地消息真正会用到的 `u8 / u16 / u32 / string / bytes / bool-like-u8`
- 当前没有把 `float`、`int16_t`、定位类 TLV 作为第一版主推接口提前做进来
- 字符串读取接口 `Protocol_Tlv_ReadStringView(...)` 返回的是“只读视图”，不会自动补 `\\0`

### 6.3 ACK 相关

- `Protocol_AppAck_Build(...)`
- `Protocol_AppAck_Parse(...)`
- `Protocol_AppAck_MatchesRequest(...)`

这层已经把 `TLV_ACK_FUNC / TLV_RESULT / TLV_REASON` 统一封装好了，业务代码不需要再自己手拼 ACK。

### 6.4 调度发送

- `Service_RetryAckScheduler_Enqueue(...)`
- `Service_RetryAckScheduler_SetEventCallback(...)`
- `Service_RetryAckScheduler_Cancel(...)`
- `Service_RetryAckScheduler_Reset(...)`
- `Service_RetryAckScheduler_HasPending(...)`
- `Service_RetryAckScheduler_PendingCount(...)`

### 6.5 接收分发

- `Service_Dispatcher_Register(...)`
- `Service_Dispatcher_SetDefault(...)`
- `Service_Dispatcher_DispatchRaw(...)`
- `Service_Dispatcher_DispatchFrame(...)`

`DispatchFrame(...)` 适合上层已经自己完成预处理、过滤或转发的场景。

## 7. 关于消息序号和 ACK

约定如下：

- `seq == 0x00`：默认认为此消息不要求 ACK
- `seq != 0x00`：默认认为此消息要求 ACK

调度器的当前行为：

- 需要 ACK 的消息在入队时自动分配非零 `seq`
- 如果发送成功并收到匹配 ACK，才推进下一个 `seq`
- `seq` 溢出后回到 `0x01`

ACK 匹配规则当前已经提升为：

- `func_code == MSG_ACK`
- `ACK.seq == request.seq`
- 若 ACK 中带 `TLV_ACK_FUNC`，则必须和原消息 `func_code` 一致

这比只按 `seq` 匹配更安全。

补充说明：

- 当前 `TLV_RESULT` 已按 `通信协议v2` 表格修正为 `0x20`
- `0x07` 在协议里对应的是 `TLV_DELTA`，但它属于当前暂未主推的预留事件链路能力

## 8. 当前边界与容量

### 8.1 应用层边界

- `data_len` 最大 `255 byte`
- 当前应用层完整编码后最大 `263 byte`
- 协议里的“数据长度”表示字节数，不是位数

### 8.2 ISO-TP 边界

- `TRANSPORT_ISOTP_MAX_MESSAGE_SIZE = 512`

所以当前栈已经完全覆盖你们现有协议最大帧需求。

如果未来应用层总长超过 `263 byte`，那不是只改 ISO-TP 就够，还必须同步改应用层协议头里的长度字段设计。

## 9. 必须显式填写优先级

一旦有调度器，应用层就不能假设“谁先调用谁先发”。

上层每次入队都应明确给出：

- `priority`
- `require_ack`
- `retry_count`
- `retry_timeout_ms`

这样移植到别的 MCU、别的任务模型后，调度行为才是一致的。

建议分配：

- 控制命令：`HIGH / HIGHEST`
- 状态回报：`MEDIUM`
- 心跳和调试：`LOW`
- 故障上报：`HIGHEST`

## 10. 怎么移植到别的 MCU

### 10.1 推荐最小改动路径

优先保留以下目录不动：

- `transport`
- `protocol`
- `service`

优先重写或检查以下文件：

- [app_can.c](/E:/STM32_PROJ/F407yuntai/F407_LOOP_ISOTP_TEST/F407_LOOP_ISOTP_TEST/Core/Src/app_can.c:1)
- [app_log.c](/E:/STM32_PROJ/F407yuntai/F407_LOOP_ISOTP_TEST/F407_LOOP_ISOTP_TEST/Core/Src/app_log.c:1)
- 硬件初始化文件 `can.c / usart.c / gpio.c`

核心原则：

- `transport / protocol / service` 视为平台无关库，换 F1/F4/L4 等时尽量不要改这些层
- `app_can.c / app_log.c` 才是平台适配层，用来隔离 HAL、寄存器和板级差异
- 如果每换一块板都要改 `isotp.c` 或 `retry_ack_scheduler.c`，说明分层已经被破坏

### 10.2 裸机移植步骤

1. 初始化硬件时钟、GPIO、CAN、UART。
2. 实现 `App_Can_Init()`、`App_Can_Send()`、`App_Can_SetRxCallback()`、`App_Can_GetDiag()`。
3. 初始化一个或多个 `TransportIsotpContext`。
4. 初始化 `ServiceDispatcher`。
5. 初始化 `ServiceRetryAckScheduler`。
6. 在主循环里持续调用：
   - `Transport_Isotp_Poll(...)`
   - `Service_RetryAckScheduler_Poll(...)`
7. 在 CAN RX 回调里把收到的原始 CAN 帧喂给：
   - `Transport_Isotp_OnCanFrame(...)`

移植到 F1/F4/L4 或其他板子时，最常需要微调的点：

1. CAN 滤波器银行或接收 FIFO 分配方式。
2. 单 CAN / 双 CAN 实例映射关系。
3. 500 kbps 波特率与当前 APB 时钟是否匹配。
4. 错误中断、接收中断的注册方式。
5. 串口日志是否继续阻塞发送，还是改成 DMA/后台任务。

### 10.3 RTOS 移植建议

1. CAN 接收中断里只做取帧和投递，不做复杂业务。
2. 在通信任务里统一处理：
   - `Transport_Isotp_OnCanFrame(...)`
   - `Transport_Isotp_Poll(...)`
   - `Service_RetryAckScheduler_Poll(...)`
   - `Service_Dispatcher_DispatchRaw(...)`
3. `get_ms` 必须提供单调递增毫秒时基。
4. `send_fn / idle_fn` 的线程安全语义要由你的任务模型保证。

## 11. Demo 主循环行为

入口文件：

- [main.c](/E:/STM32_PROJ/F407yuntai/F407_LOOP_ISOTP_TEST/F407_LOOP_ISOTP_TEST/Core/Src/main.c:1)

当前主循环演示的是：

- 双 CAN 真实总线互发
- ISO-TP 自动拆包/组包
- ACK 自动应答
- 需要 ACK 的消息自动重试
- 每秒打印 CAN 健康状态

这部分是示例业务，不应被其他平台直接复制成“最终业务逻辑”。

## 12. 日志与分析工具

日志分析脚本：

- [tools/log_analyzer.py](/E:/STM32_PROJ/F407yuntai/F407_LOOP_ISOTP_TEST/F407_LOOP_ISOTP_TEST/tools/log_analyzer.py:1)

作用：

- 在线采集串口日志
- 给每一行日志自动补主机时间戳
- 离线分析历史日志
- 统计错误、ACK、调度器事件、分发事件
- 分析 `CAN1 / CAN2` 的周期、抖动和偏差

脚本有两种模式：

- `analyze`
  - 读取已经保存好的日志文件并分析
- `live`
  - 直接监听串口，边采集边分析

### 12.1 使用前准备

如果你的 Python 环境还没有串口库，`live` 模式需要先安装：

```bash
pip install pyserial
```

如果只做离线分析，不需要安装 `pyserial`。

### 12.2 在线采集模式

在线采集 20 秒，并且从“收到第一帧日志”开始计时：

```bash
python tools/log_analyzer.py live --port COM11 --baud 921600 --duration 20 --start-on-first-frame --save C:\Users\24705\Desktop\live_log.txt
```

常用参数说明：

- `--port`
  - 串口号，当前你们常用的是 `COM11`
- `--baud`
  - 串口波特率，当前 demo 为 `921600`
- `--duration`
  - 统计窗口时长，单位秒
- `--start-on-first-frame`
  - 推荐开启
  - 含义是只有收到第一条日志后才开始算 20 秒，避免板子刚上电还没开始发包时把空等时间也算进去
- `--save`
  - 把带主机时间戳的日志保存到文件，后续可以继续离线分析
- `--max-wait-first-frame`
  - 可选
  - 如果担心一直等不到第一帧，可以设置最大等待时间

在线模式推荐阅读流程：

1. 先确认板子串口已经连到电脑，并且串口参数正确。
2. 运行 `live` 模式开始监听。
3. 看到 `First frame received` 后，说明正式统计窗口已经开始。
4. 等脚本结束后，先看 `Summary` 和 `Errors`。
5. 再看 `Timing` 判断 1000 ms / 200 ms 是否接近目标值。
6. 最后看 `Sequence Usage`、`Scheduler Events`、`Dispatch Counts` 判断收发闭环是否正常。

### 12.3 离线分析模式

离线分析：

```bash
python tools/log_analyzer.py analyze --file C:\Users\24705\Desktop\live_log.txt
```

如果日志不是 `utf-8`，可以额外指定编码：

```bash
python tools/log_analyzer.py analyze --file C:\Users\24705\Desktop\live_log.txt --encoding utf-8
```

如果你修改了 demo 周期，也可以把期望周期传进去，让偏差计算更准确：

```bash
python tools/log_analyzer.py analyze --file C:\Users\24705\Desktop\live_log.txt --can1-period-ms 1000 --can2-period-ms 200
```

在线采集 20 秒并从第一帧开始统计：

```bash
python tools/log_analyzer.py live --port COM11 --baud 921600 --duration 20 --start-on-first-frame --save C:\Users\24705\Desktop\live_log.txt
```

这个脚本适合做：

- 错误统计
- ACK 统计
- 帧率与周期偏差分析
- `CAN1 / CAN2` 定时是否接近设定值

### 12.4 如何阅读分析结果

脚本输出通常包含这些区块：

- `Summary`
  - `total_lines`
    - 本次共分析了多少行日志
  - `error_lines`
    - 被识别为错误/超时/拒绝的日志行数量
  - `ack_tx_count`
    - ACK 发送总次数
- `Latest Health`
  - 显示每个 CAN 最后一组健康状态
  - 重点看 `TEC / REC / BOFF`
- `Latest Counters`
  - 显示当前累计 `TX_OK / TX_FAIL / RX_IRQ / RX_FRAME`
  - 用来判断总线是否持续正常收发
- `Scheduler Events`
  - 看消息是否真正经历了 `ENQUEUED / SENT / ACK_OK / RETRYING / TIMEOUT_DROPPED`
- `Dispatch Counts`
  - 看上层是否真的收到了并分发了目标功能码
- `Errors`
  - 集中列出超时、发送失败、ACK_REJECTED 等错误
- `Timing`
  - 这是最重要的区块之一
  - 里面会给出平均周期、最小周期、最大周期、标准差
  - 如果带 `target=... delta=...`，就说明脚本还帮你算了相对目标周期的偏差
- `Sequence Usage`
  - 用来看 `seq` 是否真的在变化
  - 对需要 ACK 的消息尤其重要
- `Top Repeated Lines`
  - 用来看哪些日志反复出现
  - 很适合快速发现“某个错误在刷屏”或者“某个功能码一直在重发”

### 12.5 调试时应该重点看什么

如果怀疑定时不准，重点看：

- `Timing`
- `CAN1_ENQUEUE_0x51`
- `CAN2_ENQUEUE_0x02`

如果怀疑 ACK 逻辑有问题，重点看：

- `Scheduler Events`
- `CAN1:ACK_OK:0x51`
- `CAN1:RETRYING:0x51`
- `CAN1:TIMEOUT_DROPPED:0x51`
- `Sequence Usage`

如果怀疑总线物理层或收发器异常，重点看：

- `Latest Health`
- `Latest Counters`
- `Errors`
- 是否出现 `BOFF != 0`
- 是否出现 `TX_FAIL > 0`

如果怀疑应用层根本没收到消息，重点看：

- `Dispatch Counts`
- `Top Repeated Lines`
- 是否只有 `TX` 没有后续 `DISPATCH`

### 12.6 关于时间统计的注意事项

- 没有时间戳的普通串口日志，脚本无法准确计算真实周期和抖动。
- 只有 `live` 模式，或者分析“已经由脚本保存、每行带主机时间戳”的日志文件时，`Timing` 才真正有意义。
- 如果 `Timing` 区块提示 `No timestamped lines detected`，说明当前日志只能做事件统计，不能做准确周期分析。

## 13. 当前安全机制与报错机制

当前已经具备：

- 空指针检查
- 应用层最大长度检查
- TLV 写入容量保护
- ISO-TP 最大消息长度保护
- 发送超时保护
- ACK 超时重试与丢弃
- CAN 错误寄存器状态输出
- ACK 与原消息的 `func + seq` 匹配

返回值体系：

- `AppCanStatus`
- `TransportIsotpStatus`
- `ServiceRetryAckStatus`

建议业务层不要忽略这些返回值。

## 14. 当前仍需注意的维护边界

下面这些不是 bug，但属于需要文档化的工程边界：

1. `app_log.c` 现在仍然使用阻塞式 `HAL_UART_Transmit`。
2. 在高日志量下，它仍可能影响实时性；当前通过默认关闭逐帧日志来减轻影响。
3. `retry_ack_scheduler` 当前每个实例只支持单个 in-flight ACK 等待窗口，不是多窗口滑动发送器。
4. `main.c` 里的 Demo 业务逻辑仍然是示例层，不应与协议栈库层耦合扩散。
5. `app_can.c` 目前仍是 STM32 HAL 适配实现，跨平台时必须重写这一层。
6. 第一版 API 是按“当前已落地消息”裁剪的，不代表 `通信协议v2` 所有预留消息都已经配齐 helper。

## 15. 给应用层的使用方式

应用层开发者应尽量只做下面几件事：

1. 继续完善通讯协议，完善功能码 和 TLV 语义。
2. 组装 `ProtocolAppFrame`。
3. 按需填写优先级、ACK、重试参数后入队。
4. 在 dispatcher 注册自己的业务处理函数。

不建议应用层直接处理：

- CAN 单帧收发
- ISO-TP 分包细节
- ACK 超时状态机
- 底层错误寄存器细节

这样才能让后续平台迁移真正省时间。
