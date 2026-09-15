"""Deterministic tests of real encoder delta logic via Unix counter injection."""
# MP_ERANGE=34 is defined in py/mperrno.h but not exported by errno.
import gc
from smartcar import encoder, ADC_Group, _encoder_step


def rejects(exc, fn, *args):
    try:
        fn(*args)
    except exc:
        return
    raise AssertionError("expected rejection")


def overflow(enc):
    try:
        enc.read()
    except OSError as error:
        assert error.args[0] == 34
    else:
        raise AssertionError("ambiguous half-range accepted")


# These changes would break the tests: unsigned deltas, resetting CNT after
# capture, incorrect sign inversion, or truncating the TIM2 counter to 16 bits.
for pins, bits in ((('A6', 'A7'), 16), (('A0', 'A1'), 32)):
    half = 1 << (bits - 1)
    for inverted in (False, True):
        sign = -1 if inverted else 1
        enc = encoder(pins[0], pins[1], inverted)
        _encoder_step(enc, -1)
        assert enc.get() == 0
        assert enc.read() == -sign  # wraps down from zero
        _encoder_step(enc, 2)
        assert enc.read() == 2 * sign  # wraps up through zero
        assert enc.read() == 0
        _encoder_step(enc, half - 1)
        assert enc.read() == (half - 1) * sign
        _encoder_step(enc, -(half - 1))
        assert enc.read() == -(half - 1) * sign
        _encoder_step(enc, -half)
        overflow(enc)
        # Overflow consumes the ambiguous interval and permits recovery.
        _encoder_step(enc, 1)
        assert enc.read() == sign
        gc.collect()
        _encoder_step(enc, -2)
        enc.capture()
        assert enc.get() == -2 * sign
        enc.deinit()
        enc.deinit()

# Reject reversed channels and incompatible hardware mappings.
rejects(ValueError, encoder, 'A7', 'A6')
rejects(ValueError, encoder, 'A0', 'A7')
rejects(ValueError, encoder, 'A0', 'A0')

# Failed timer claim must leave the existing owner's timer intact.
enc = encoder('A0', 'A1')
rejects(OSError, encoder, 'A5', 'A1')
_encoder_step(enc, 123456)
assert enc.read() == 123456
enc.deinit()
enc = encoder('A5', 'A1')
assert enc.read() == 0
enc.deinit()

# Failed SECOND pin claim must release the first pin and the timer.
adc = ADC_Group(1)
adc.addch('A1')
rejects(OSError, encoder, 'A0', 'A1')
adc.addch('A0')  # proves first pin was rolled back
adc.deinit()
enc = encoder('A0', 'A1')  # proves timer was rolled back
_encoder_step(enc, 9)
gc.collect()
assert enc.read() == 9
enc.deinit()

# Failed FIRST pin claim also releases the timer.
adc = ADC_Group(1)
adc.addch('A0')
rejects(OSError, encoder, 'A0', 'A1')
adc.deinit()
enc = encoder('A0', 'A1')
enc.deinit()
print('smartcar encoder tests passed')
