"""Run on H743 with an IMU on SPI2 B13/B14/B15, CS B12, 3.3V/GND.

Not a Unix test: checks reservations against existing Python SPI handles.
"""
import pyb
from machine import SPI
from seekfree import IMU660RX
from smartcar import ticker


def busy(fn, *args, **kwargs):
    try:
        fn(*args, **kwargs)
    except OSError as error:
        assert error.args[0] == 16
    else:
        raise AssertionError('expected EBUSY')


legacy = pyb.SPI(2)
spi = SPI(2)
spi.deinit()
imu = IMU660RX()
pit = ticker(1)
try:
    busy(SPI, 2)
    busy(pyb.SPI, 2)
    busy(spi.init, baudrate=100000)
    busy(spi.deinit)
    busy(spi.write, b'\x00')
    busy(legacy.init, pyb.SPI.CONTROLLER)
    busy(legacy.deinit)
    busy(legacy.send, b'\x00')
    print('Initial frame:', imu.read())
    pit.capture_list(imu)
    busy(imu.deinit)
    pit.callback(lambda t: None)
    pit.start(10)
    busy(imu.capture)
    busy(imu.read)
    pyb.delay(100)
    assert pit.ticks() >= 5
    print('Ticker frame:', imu.get())
finally:
    pit.stop()
    pit.capture_list()
    pit.callback(None)
    imu.deinit()
spi.init(baudrate=100000)
spi.deinit()
print('IMU SPI ownership checks passed')
