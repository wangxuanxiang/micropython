"""Quadrature encoder: A -> A6/TIM3_CH1, B -> A7/TIM3_CH2; common GND."""
from smartcar import encoder, ticker
import time

enc = encoder('A6', 'A7', invert=False)
pit = ticker(2)

def callback(t):
    pass

try:
    pit.capture_list(enc)
    pit.callback(callback)
    pit.start(10)
    while True:
        # Most recent 10 ms interval, not position or the last 100 ms total.
        print(pit.ticks(), enc.get())
        time.sleep_ms(100)
finally:
    pit.stop()
    pit.capture_list()
    pit.callback(None)
    enc.deinit()
