# Smartcar H743 implementation plan (confirmed scope)

Implement ticker, ADC_Group and encoder together, preserving SeekFree method
names. Windows is authoritative; synchronize only task files into WSL Ubuntu.

## Final design

- Four logical tickers (0..3) share TIM7 at 1 ms. Each serviced trigger captures
  native sensors serially in C IRQ, then schedules/coalesces a Python callback.
  Per-start dispatch identity invalidates stale callbacks. ticks resets on start.
- ADC1/2 regular scan sequences, 12-bit, AUTDLY rank polling, software averaging.
  Capture is bounded and allocation-free; sample timings are H743-specific.
- Encoder uses hardware CH1/CH2 quadrature x4 with modular signed delta; pulse
  plus direction is explicitly deferred. Reserve TIM5/6 and board storage pins.
- Resource claims protect smartcar objects, pyb.Timer and machine.PWM timers,
  and ordinary ADC setup/read paths. Other GPIO/peripheral sharing is prohibited.
- Unix shares object/resource/delta logic but supplies deterministic time/input;
  it cannot validate analog conversion, electrical counting or IRQ jitter.
- E11/E12/E13 examples use E3/C13, A0/A1 ADC and A6/A7 TIM3 encoder respectively.

## Execution

- [x] Missing-module RED test before implementation.
- [x] Implement native module, drivers, roots and soft-reset lifecycle.
- [x] Enable board build without changing user's BOARD setting.
- [x] Compare/back up/synchronize task destinations into WSL.
- [x] Unix ticker, encoder and extended ADC tests pass.
- [x] BOARD submodules succeeds; independent-directory ARM firmware links.
- [x] Add adapted demos and complete API/difference documentation.
- [x] Final review fixes, full targeted verification and firmware export.
- [ ] Physical hardware verification (requires flashed board and signals).

Verification details and commands: modules/smartcar/README.md.
