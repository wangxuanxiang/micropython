"""WeAct H743: SPI2 B13 SCK, B15 MOSI, B14 MISO, B12 CS; 3.3 V/GND."""
from machine import Pin
from smartcar import ticker
from seekfree import IMU660RX
import time

led = Pin('E3', Pin.OUT)
key = Pin('C13', Pin.IN, Pin.PULL_DOWN)
IMU660RX.help()
print('Initialising IMU...')
imu = IMU660RX(1)
imu.info()
imu_data = imu.get()  # Same six-int list is refreshed by the native capture.
pit = ticker(1)
pending = False

def callback(t):
    global pending
    if t.ticks() % 20 == 0:
        pending = True

try:
    # First verify the immediate read before enabling automatic acquisition.
    print('First sample:', imu.read())
    pit.capture_list(imu)
    pit.callback(callback)
    pit.start(10)
    while not key.value():
        pit.ticks()  # Surfaces a TIM7 acquisition-budget error.
        if pending:
            pending = False
            led.value(not led.value())
            print('acc = {:6d}, {:6d}, {:6d}'.format(*imu_data[:3]))
            print('gyro= {:6d}, {:6d}, {:6d}'.format(*imu_data[3:]))
            imu.get()  # Surfaces a failed SPI read; last good data stays cached.
        time.sleep_ms(1)
finally:
    pit.stop()
    pit.capture_list()
    pit.callback(None)
    imu.deinit()
    led.value(0)

