# softpwm.py
from machine import Pin
from smartcar import ticker

class SoftPWM:
    """
    基于 PIT ticker 的软件 PWM
    原理：定时器以固定周期 tick 累加计数，
         把 0~period 的计数区间按 duty 阈值翻转引脚。
    """
    def __init__(self, pin_name, freq=1000, duty=0):
        """
        :param pin_name: 引脚名，如 'E3'
        :param freq:     PWM 频率 (Hz)
        :param duty:     占空比 0.0 ~ 1.0
        """
        self.pin = Pin(pin_name, Pin.OUT)
        self.freq = freq
        self.duty = max(0.0, min(1.0, duty))
        # 一个 PWM 周期分成 PERIOD 个 tick
        self.PERIOD = 100                 # 分辨率：100 级
        self._counter = 0
        self._enable = True
        # 计算 ticker 的中断周期（ms）
        # tick_ms = 1000 / (freq * PERIOD)
        tick_ms = max(1, int(1000 / (freq * self.PERIOD)))
        self.tick_ms = tick_ms

        self._apply()

    # ---------- 内部：更新输出电平 ----------
    def _apply(self):
        if not self._enable:
            self.pin.value(0)
            return
        threshold = int(self.duty * self.PERIOD)
        self.pin.value(1 if self._counter < threshold else 0)
    # ---------- 用户接口 ----------
    def set_duty(self, duty):
        """设置占空比 0.0 ~ 1.0"""
        self.duty = max(0.0, min(1.0, duty))
    def set_freq(self, freq):
        """设置频率（需重建 ticker，见 PWMController）"""
        self.freq = freq
    def enable(self, en=True):
        self._enable = en
        if not en:
            self.pin.value(0)
    def deinit(self):
        self.pin.value(0)


class PWMController:
    """
    管理多路 SoftPWM，共用同一个 PIT ticker。
    所有通道必须使用相同的频率/分辨率。
    """

    def __init__(self, freq=1000, period=100,ticker_id = 1):
        self.freq = freq
        self.PERIOD = period
        self.channels = []
        self._counter = 0
        self._tick_ms = max(1, int(1000 / (freq * period)))
        self.ticker = ticker(ticker_id)          # 用 PIT 定时器
        self.ticker.callback(self._on_tick)

    def add(self, pin_name, duty=0.0):
        ch = SoftPWM(pin_name, self.freq, duty)      # 不走 __init__，复用共享 ticker
        ch.PERIOD = self.PERIOD
        ch._enable = True
        ch._counter = 0
        ch._apply = lambda: ch.pin.value(
            1 if (ch._enable and self._counter < int(ch.duty * self.PERIOD)) else 0
        )
        self.channels.append(ch)
        ch._apply()
        return ch

    def _on_tick(self, t):
        self._counter += 1
        if self._counter >= self.PERIOD:
            self._counter = 0
        for ch in self.channels:
            ch._apply()

    def start(self):
        self.ticker.start(self._tick_ms)
    def stop(self):
        self.ticker.stop()
        for ch in self.channels:
            ch.pin.value(0)

    def set_duty(self, ch, duty):
        ch.duty = max(0.0, min(1.0, duty))


# ---------------- 简单用法示例 ----------------
if __name__ == "__main__":
    import time
    pwm = PWMController(freq=1000, period=100, ticker_id=1)   # 1kHz, 100 级
    led = pwm.add('E3', duty=0.1)
    pwm.start()
    print("PWM started")
    d = 0
    step = 0.05
    while True:
        led.set_duty(d)
        d += step
        if d > 1.0:
            d = 1.0
            step = -step
        if d < 0.0:
            d = 0.0
            step = -step
        time.sleep_ms(100)
