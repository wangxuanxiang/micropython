"""WeAct H743: TIM7 drives ticker; E3 LED, C13 active-high button."""
from machine import Pin
from smartcar import ticker
import time

led = Pin('E3', Pin.OUT)
key = Pin('C13', Pin.IN, Pin.PULL_DOWN)
pit = ticker(0)

def callback(t):
    led.value(not led.value())

pit.callback(callback)
try:
    pit.start(100)
    while not key.value():
        time.sleep_ms(20)
finally:
    pit.stop()
    pit.capture_list()
    pit.callback(None)
    led.value(0)
