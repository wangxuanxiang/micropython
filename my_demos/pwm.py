#here is an example of encoder
from machine import Pin
import time,gc

'''初始化led,配置E3引脚为输出模式'''
led = Pin('E3',Pin.OUT)
print("led_init_ed")
flag = 0
while True:
    gc.collect()
    time.sleep_ms(500)
    flag = 1 - flag
    led.value(flag)
