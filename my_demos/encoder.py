#here is an example of encoder
from machine import Pin
from smartcar import ticker, encoder
import time,gc

'''初始化led,配置E3引脚为输出模式'''
led = Pin('E3',Pin.OUT)
print("led_init_ed")

'''初始化编码器,设置正负极性（默认为False）'''
encoder1 = encoder("A6","A7",True)
encoder2 = encoder("A8","A9",True)
print("encoder_init_ed")

'''定义定时器回调函数'''
def callback(t):
    enc_data1 = encoder1.get()
    enc_data2 = encoder2.get()
    print(f"{enc_data1},{enc_data2}")
    
'''初始化定时器'''
pit = ticker(1)
pit.capture_list(encoder1,encoder2)
pit.callback(callback)
pit.start(5)
flag = 1

while True:
    gc.collect()
    time.sleep_ms(500)
    flag = 1 - flag
    led.value(flag)

