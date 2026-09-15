"""Unix API/transactional state tests; does not emulate analog conversion."""
from smartcar import ADC_Group, ticker, encoder, _adc_set, _advance
import gc

def rejects(exc, fn, *args, **kwargs):
    try:
        fn(*args, **kwargs)
    except exc:
        return
    raise AssertionError('expected rejection')

for value in (-1, 0, 3, 1 << 32):
    rejects((ValueError, OverflowError), ADC_Group, value)
a = ADC_Group(1)
b = ADC_Group(2)
rejects(OSError, ADC_Group, 1)
rejects(OSError, a.read)  # no channels
rejects(ValueError, b.addch, 'A0')
a.addch('A2')
rejects(OSError, b.addch, 'A2')
b.addch('A3')
_adc_set(a, [100])
_adc_set(b, [200])
assert a.read() == [100] and b.read() == [200]
rejects((ValueError, OverflowError), a.init, 1, average=(1 << 32) + 1)
rejects((ValueError, OverflowError), a.init, 1, period=1 << 32)
_adc_set(a, [300])
rejects(ValueError, _adc_set, a, [-1])
assert a.read() == [300]  # failed update leaves data intact
pit = ticker(0)
other = ticker(1)
pit.capture_list(a, b)
rejects(OSError, other.capture_list, b)
rejects(ValueError, pit.capture_list, a, a)
rejects(OSError, a.init, 1)
pit.callback(lambda t: None)
pit.start(5)
rejects(OSError, a.read)
rejects(OSError, b.capture)
rejects(OSError, pit.capture_list)
gc.collect()
_advance(5)
assert a.get() == [300] and b.get() == [200]
pit.stop()
pit.capture_list()
a.deinit()
b.deinit()
a = ADC_Group(1)
pins = ('A0','A1','A2','A3','A4','A5','A6','A7',
        'B0','B1','C0','C1','C2','C3','C4','C5')
for p in pins:
    a.addch(p)
rejects(ValueError, a.addch, 'E3')
values = [i * 257 for i in range(16)]
_adc_set(a, values)
assert a.read() == values
result = a.get()
result[0] = 999
assert a.get() == values
a.deinit()
print('smartcar ADC tests passed')
