try:
    from smartcar import ticker, ADC_Group, encoder, _advance, _adc_set, _encoder_step
except ImportError:
    raise AssertionError("smartcar C modules not implemented")
import gc

def rejects(exc, fn, *args, **kwargs):
    try:
        fn(*args, **kwargs)
    except exc:
        return
    raise AssertionError("expected rejection")

rejects(ValueError, ticker, -1)
rejects(ValueError, ticker, 4)
pit = ticker(1)
assert pit is ticker(1)
rejects(ValueError, pit.start, 10)
rejects(TypeError, pit.callback, 42)
pit.callback(lambda t: None)
rejects(ValueError, pit.start, 0)
rejects(TypeError, pit.capture_list, object())
rejects(ValueError, ADC_Group, 0)
rejects(ValueError, ADC_Group, 3)
adc = ADC_Group(1)
adc.init(1, period=ADC_Group.PMODE3, average=ADC_Group.AVG16)
adc.addch("A0")
adc.addch("A1")
rejects(ValueError, adc.addch, "A0")
rejects(ValueError, adc.addch, "E3")
rejects(ValueError, adc.init, 2)
rejects(ValueError, adc.init, 1, average=3)
_adc_set(adc, [123, 4095])
assert adc.get() == [0, 0]
assert adc.read() == [123, 4095]
snapshot = adc.get()
_adc_set(adc, [321, 0])
adc.capture()
assert adc.get() == [321, 0]
assert snapshot == [123, 4095]
rejects(ValueError, _adc_set, adc, [4096, 0])
rejects(ValueError, _adc_set, adc, [1])
enc = encoder("A6", "A7", invert=True)
_encoder_step(enc, 10)
assert enc.get() == 0
enc.capture()
assert enc.get() == -10
assert enc.read() == 0
_encoder_step(enc, -5)
assert enc.read() == 5
rejects(ValueError, encoder, "E3", "C13")
rejects(OSError, encoder, "A6", "A7")
events = []
pit.capture_list(adc, enc)
rejects(TypeError, pit.capture_list, adc, object())
rejects(ValueError, pit.capture_list, *([adc] * 9))
pit.callback(lambda t: events.append((t is pit, t.ticks(), adc.get(), enc.get())))
pit.start(10)
_adc_set(adc, [100, 200])
_encoder_step(enc, 7)
gc.collect()
_advance(9)
assert pit.ticks() == 0 and events == []
_advance(1)
assert events == [(True, 1, [100, 200], -7)]
rejects(OSError, adc.deinit)
pit.stop()
_advance(30)
assert pit.ticks() == 1 and len(events) == 1
pit.start(5)
_advance(5)
assert pit.ticks() == 1 and len(events) == 2
pit.stop()
pit.capture_list()
adc.deinit()
enc.deinit()
rejects(ValueError, adc.capture)
rejects(ValueError, enc.read)
rejects(ValueError, pit.capture_list, adc)
events.clear()
for i in range(4):
    t = ticker(i)
    t.callback(lambda t: events.append(t))
    t.start(10)
_advance(10)
assert events == [ticker(0), ticker(1), ticker(2), ticker(3)]
for i in range(4):
    ticker(i).stop()
events.clear()
def stopping(t):
    events.append(t.ticks())
    t.stop()
pit.callback(stopping)
pit.start(1)
_advance(5)
assert events == [1]
adc = ADC_Group(1)
adc.addch("A0")
adc.deinit()
enc = encoder("A6", "A7")
_encoder_step(enc, -1)
assert enc.read() == -1
_encoder_step(enc, 2)
assert enc.read() == 2
enc.deinit()
print("smartcar tests passed")
