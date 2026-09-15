# WeAct STM32H743 smartcar

本模块用 C 实现 `smartcar.ticker`、`ADC_Group` 和 `encoder`，在
`WEACTSTUDIO_MINI_STM32H743` 构建中自动启用。Unix 使用相同的对象、资源管理、
缓存和回调逻辑，底层输入与时钟由模拟后端提供；Unix 测试不能验证电气或中断时序。

## 调用链

`from smartcar import ...` → `MP_REGISTER_MODULE` 注册表 →
`modsmartcar.c` 的 Python 对象/方法 → `adc_port.c` / `encoder_port.c` →
STM32 HAL 初始化、ADC/TIM/GPIO 寄存器 → 采样缓存 → Python `get()` 返回值。

TIM7 每 1 ms 进入 C 中断，依次检查 ticker 0..3。到期后按 capture_list
顺序直接调用 C 驱动采样，再调度 Python 回调。四个逻辑 ticker 共用 TIM7，
因此给编码器保留 TIM2/TIM3 等计数器。Python 回调不在硬中断中执行。

## 接口

```python
from smartcar import ticker, ADC_Group, encoder
adc = ADC_Group(1)
adc.init(1, period=ADC_Group.PMODE3, average=ADC_Group.AVG16)
adc.addch('A0')
adc.addch('A1')
enc = encoder('A6', 'A7', invert=False)
pit = ticker(1)
pit.capture_list(adc, enc)
pit.callback(lambda t: None)
pit.start(10)
# adc.get(): 最近一次采集的列表；enc.get(): 最近一次采集间隔的有符号计数
# 清理时：
pit.stop()
pit.capture_list()
pit.callback(None)
adc.deinit()
enc.deinit()
```

- `ticker(id)`：0..3，同 ID 返回同一对象。先设置 callable 回调再 start(ms)。
  正整数毫秒，建议至少 5 ms，并给全部 ADC 转换留出余量。
- `ticks()`：本次 start 后实际处理的采集触发次数，start 重置为 0，stop 保留。
  32 位计数自然回绕。它不是已执行的 Python 回调次数。
- `capture_list(*sensors)`：最多 8 个原生传感器，采集顺序等于传入顺序。
  运行时不能改列表。空参数清空；禁止重复或跨 ticker 共享同一对象。
- `callback(fn)`：更换回调会停止当前运行，再次 start 生效；None 清除回调。
  每个 ticker 最多一个待执行回调，繁忙时合并通知，采样继续。
  调度队列满会丢失当次通知。回调异常会打印，ticker 继续运行。
  stop/restart 使旧队列通知失效。get() 返回执行时的最新缓存，不能作为历史队列。
- `ADC_Group(1或2)`：每个硬件 ADC 一个组，最多 16 个不同通道。
  addch 顺序就是 get 列表顺序。12 位 0..4095；get 返回独立列表。
  PMODE0/1/2/3 对应 8.5/32.5/64.5/387.5 个 ADC 时钟周期。
  AVG1/4/8/16/32 是完整扫描序列的软件平均，采用整数除法。
  这里是规则序列扫描及逐通道读取，没有 DMA。
- `encoder(A, B, invert=False)`：A/B 必须是同一定时器 CH1/CH2，正交 x4
  计数。capture 更新间隔增量，get 读缓存；read 等价于 capture + get。
  通过模计数差值处理回绕，不在采样后清零硬件计数器。
  TIM3 等 16 位计数器每个采集间隔必须小于 32768 个边沿；TIM2 为 32 位，
  限制为 2147483648。恰好半范围报 `OSError(34)`，其余超范围无法可靠检测。
- 两类传感器都支持 capture/get/read/deinit。已绑定运行 ticker 时不能手动
  capture/read；deinit 前必须从所有 capture_list 解绑。对象有 GC 根保护，
  删除 Python 变量不会释放硬件，需显式 deinit 或软复位。

## 板级引脚与资源

同时使用可选 ADC1 A0/A1 + 编码器 A6/A7(TIM3)；ADC2 可用 A2/A3。
PA1 是 ADC1 通道17，不能误用 CSV 中属于 PA1_C 的通道1。
编码器还可选择 A0/A1(TIM2)、A8/A9(TIM1)、C6/C7(TIM3)，但不能与 ADC 重用引脚。
硬件 AF 表决定完整支持范围，Unix 模拟只覆盖测试用的部分有效引脚组合。

TIM5/TIM6 保留给系统，ticker 占 TIM7。Flash/QSPI、USB、SWD、晶振引脚
不允许被本模块占用；因此不能照搬其他板子的 B6/B7、D12/D13 编码器接线。
LED E3，按键 C13，高电平按下。所有外部信号电压应符合板卡/芯片规格，示例使用 3.3 V。

定时器运行时拒绝接管，原有 pyb.Timer/TimerChannel 在 smartcar 占用期间也会拒绝访问；machine.PWM 的构造和修改同样受保护。
ADC_Group 存活期间普通 machine/pyb ADC 构造及共享配置会拒绝，以保护 ADC 公共时钟。
不能用 Pin/UART/SPI/PWM 或直接寄存器访问重新配置已分配给 smartcar 的引脚。
采集期间不要改变 machine.freq。资源注册表不是整个 MicroPython 的全局引脚仲裁器。

ADC 捕获在硬中断中有界轮询，异常转换最长等候约 20 ms；不会调用 Python 或分配内存。
最大 16通道×AVG32×PMODE3 在 30 MHz 时仅转换约 6.8 ms，不能用于 5 ms 周期。
同一 TIM7 中断内的全部采样总时间必须小于 1 ms；超出时会停止全部运行的 ticker，
其 ticks() 抛出 ETIMEDOUT，防止连续中断使 Python 无法执行 stop()。此时需降低
通道数/PMODE/平均次数，或改为主循环 read()；单纯延长 start 周期不能绕过 1 ms 限制。
中断响应延迟仍可能丢节拍；本版本不补偿丢失时间，需实测周期抖动。软复位先停止 ticker，再释放 ADC/encoder；
若外设停止失败会打印提示，需要硬复位后复用。

## 与原 SeekFree demo/readme 的不同

| 项目 | 当前 H743 实现 |
|---|---|
| 导入、方法名 | 保留 smartcar.ticker / ADC_Group / encoder 及核心方法 |
| 引脚 | 换成 H743 引脚，原 RT1021 D 编号不能照搬 |
| ticker 底层 | 单 TIM7 分派四个逻辑通道；C 中断采样，Python 延后回调 |
| capture_list 类型 | 只实现 ADC_Group 和 encoder；其他 seekfree 传感器未接入 |
| ADC 时序、平均 | H743 采样周期；规则序列扫描，软件平均，没有 DMA |
| encoder 模式 | 只支持正交 x4；原文提到的 PLUS+DIR 尚未实现 |
| 错误与清理 | 新增占用检查、deinit、空 capture_list 解绑；错误通过异常返回 |
| Unix | 只提供确定性模拟，无真实 ADC、外部编码器信号或硬件中断 |

## 构建与验证

在 Linux 的仓库根目录运行：

```bash
make -C ports/unix BUILD=build-smartcar USER_C_MODULES=../../modules/smartcar -j4
ports/unix/build-smartcar/micropython tests/smartcar/ticker.py
ports/unix/build-smartcar/micropython tests/smartcar/encoder.py
ports/unix/build-smartcar/micropython tests/smartcar/adc.py
make -C ports/stm32 BOARD=WEACTSTUDIO_MINI_STM32H743 submodules
make -C ports/stm32 BOARD=WEACTSTUDIO_MINI_STM32H743 BUILD=build-smartcar-WEACTSTUDIO_MINI_STM32H743 -j4
```

也可使用原来的不带 BUILD 参数命令；首次加入模块后旧目录如出现 QSTR/frozen_content
冲突，使用上面的独立构建目录。固件位于对应 BUILD 目录的 firmware.hex/.dfu。
本地 Windows → WSL 的窄范围同步脚本为 `tools/smartcar_sync.py`，先检查 Linux
目标是否有独立修改，保留初始备份再同步；不会复制整个仓库。

上板示例在 `seekfree_demos/stm32/`。先测试 E11 闪灯和按键，再给 E12 接入已知
0 V/3.3 V 电压验证读数，最后用 E13 验证正反转、每转计数和实际采样周期。
这些硬件测试尚需烧录并接线执行。
