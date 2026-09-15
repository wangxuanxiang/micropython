# Verification — 2026-09-13

Built from the current Windows workspace, synced with original-file backups to
`/home/wxx33/micropython`. Board: `WEACTSTUDIO_MINI_STM32H743`.

- Unix C-module build: pass.
- `tests/smartcar/ticker.py`: pass.
- `tests/smartcar/encoder.py`: pass (16/32-bit wraps, boundaries, rollback).
- `tests/smartcar/adc.py`: pass (group isolation, cache, errors, ownership).
- E11/E12/E13 adapted demos and hardware_resources.py: mpy-cross syntax pass.
- Board submodules: pass.
- ARM firmware link and hex/bin/dfu generation: pass.
- Firmware ELF symbol check: smartcar_module, smartcar_irq, sc_adc_capture,
  sc_encoder_capture present; see symbols.txt.
- Final verification script exit code: 0. `git diff --check`: pass.

Firmware section sizes reported by linker: text 414628, data 76, bss 32856 bytes.
SHA256SUMS records checksums of all firmware artifacts.

No flashing or physical board test was performed. Unix input injection does not
verify ADC voltage/averaging, actual encoder signals, IRQ latency, the 1 ms
overload cutoff, or hardware ownership guards. Use the board demos and
`tests/smartcar/hardware_resources.py` for those checks after flashing.

Scope limitations: encoder is quadrature x4 only (no PLUS+DIR); ADC uses scan
polling and software averaging (no DMA); total capture work per TIM7 IRQ must
fit in 1 ms, otherwise all active tickers stop and ticks() raises ETIMEDOUT.
See ../../modules/smartcar/README.md for the full interface and differences.
