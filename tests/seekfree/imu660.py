from seekfree import IMU660RX, _imu660_set, _imu660_fault, _imu660_config
from smartcar import ticker, encoder, ADC_Group, _advance, _encoder_step, _adc_set
import gc

def rejects(exc, fn, *args, **kw):
    try:
        fn(*args, **kw)
    except exc:
        return
    raise AssertionError('expected rejection')

IMU660RX.help()
for model in range(3):
    _imu660_config(model, 0)
    rejects(ValueError, IMU660RX, 0)
    rejects(ValueError, IMU660RX, capture_div=65536)
    rejects(ValueError, IMU660RX, spi=1)
    imu = IMU660RX(capture_div=2)
    imu.help()
    imu.info()
    data = imu.get()
    assert data is imu.get() and data == [0] * 6
    assert len(imu.scales()) == 2
    _imu660_set(imu, [1, -2, 300, -400, 500, -600])
    assert data == [0] * 6
    imu.capture()
    assert data == [0] * 6
    imu.capture()
    assert data == [1, -2, 300, -400, 500, -600]
    old = list(data)
    _imu660_set(imu, [-32768, 32767, -3, 4, -5, 6])
    assert imu.read() is data
    assert data == [-32768, 32767, -3, 4, -5, 6]
    assert old == [1, -2, 300, -400, 500, -600]
    rejects(ValueError, _imu660_set, imu, [99, 0, 0, 0, 0, 32768])
    assert imu.read() == [-32768, 32767, -3, 4, -5, 6]
    events = []
    pit = ticker(3)
    pit.capture_list(imu)
    pit.capture_list(imu)  # replacing own attachment is legal
    rejects(ValueError, pit.capture_list, imu, imu)
    rejects(OSError, ticker(2).capture_list, imu)
    rejects(OSError, imu.deinit)
    pit.callback(lambda t: events.append(list(data)))
    pit.start(5)
    _imu660_set(imu, [11, 12, 13, 14, 15, 16])
    gc.collect()
    _advance(5)
    assert events == [[-32768, 32767, -3, 4, -5, 6]]
    _advance(5)
    assert events[-1] == [11, 12, 13, 14, 15, 16] and len(events) == 2
    rejects(OSError, imu.read)
    rejects(OSError, imu.capture)
    pit.stop()
    pit.capture_list()
    _imu660_fault(imu, 110)
    rejects(OSError, imu.read)
    rejects(OSError, imu.get)
    assert data == [11, 12, 13, 14, 15, 16]  # failed read keeps complete last frame
    _imu660_fault(imu, 0)
    assert imu.read() is data
    data.pop()
    rejects(OSError, imu.read)  # no out-of-bounds IRQ writes if user resizes
    data.append(0)
    imu.read()
    imu.deinit()
    imu.deinit()
    rejects(ValueError, imu.get)
    rejects(ValueError, imu.read)
    rejects(ValueError, imu.capture)
    rejects(ValueError, pit.capture_list, imu)
    rejects(ValueError, _imu660_set, imu, [0] * 6)

# Failure cleanup permits immediate retry, and injection checks object type.
for model, fault in ((3, 0), (0, 110), (1, 5)):
    _imu660_config(model, fault)
    rejects(OSError, IMU660RX)
_imu660_config(0, 0)
rejects(TypeError, _imu660_set, object(), [0] * 6)
imu = IMU660RX()
rejects(OSError, IMU660RX)
enc = encoder('A6', 'A7')
adc = ADC_Group(1)
adc.addch('A0')
pit = ticker(0)
pit.capture_list(imu, adc, enc)
frames = []
pit.callback(lambda t: frames.append((list(imu.get()), adc.get(), enc.get())))
_imu660_set(imu, [1, 2, 3, 4, 5, 6])
_adc_set(adc, [321])
_encoder_step(enc, -17)
pit.start(10)
_advance(10)
assert frames == [([1, 2, 3, 4, 5, 6], [321], -17)]
pit.stop()
pit.capture_list()
imu.deinit()
adc.deinit()
enc.deinit()
imu = IMU660RX(spi=4, cs='E11')
imu.deinit()
# A forced read between divided triggers must preserve their progress.
imu = IMU660RX(2)
imu.capture()
_imu660_set(imu, [7] * 6)
imu.read()
_imu660_set(imu, [8] * 6)
imu.capture()
assert imu.get() == [8] * 6
imu.deinit()
print('seekfree IMU660 tests passed')
