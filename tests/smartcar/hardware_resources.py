"""Run on the flashed H743, after soft reset; no external signals required.

Tests hardware API ownership only. Analog and encoder accuracy need wiring.
"""
from machine import Pin, PWM, ADC
from pyb import Timer
from smartcar import encoder, ADC_Group, ticker

def rejects(fn, *args, **kwargs):
    try:
        fn(*args, **kwargs)
    except (ValueError, OSError):
        return
    raise AssertionError('resource takeover accepted')

# Retain old objects to check they cannot later reconfigure the claimed timer.
old_timer = Timer(1)
old_pwm = PWM(Pin('A10'), freq=1000, duty_u16=0)
old_pwm.deinit()
old_timer.deinit()
enc = encoder('A8', 'A9')
try:
    rejects(Timer, 1, freq=1000)
    rejects(old_timer.counter, 7)
    rejects(old_timer.deinit)
    rejects(PWM, Pin('A10'), freq=1000)
    rejects(old_pwm.freq, 1000)
    rejects(old_pwm.duty_u16, 100)
    rejects(old_pwm.deinit)
finally:
    enc.deinit()

adc = ADC_Group(1)
try:
    adc.addch('A0')
    rejects(ADC, Pin('A0'))
finally:
    adc.deinit()
pit = ticker(0)
pit.callback(lambda t: None)
pit.start(10)
try:
    rejects(Timer, 7, freq=1000)
finally:
    pit.stop()
print('smartcar hardware resource tests passed')
