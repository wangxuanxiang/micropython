#!/bin/bash
# Run in WSL Ubuntu. All writes are scoped to the authorized task checkout.
set -euo pipefail
src=/mnt/d/PROJECTs/micropython
repo=/home/wxx33/micropython
python3 "$src/tools/smartcar_sync.py"
make -C "$repo/ports/unix" BUILD=build-smartcar USER_C_MODULES=../../modules/smartcar -j4 > "$src/smartcar-unix-build.log" 2>&1
for test in ticker encoder adc; do
    "$repo/ports/unix/build-smartcar/micropython" "$repo/tests/smartcar/$test.py"
done
for demo in "$repo/seekfree_demos/stm32/"*.py "$repo/tests/smartcar/hardware_resources.py"; do
    "$repo/mpy-cross/build/mpy-cross" -o /tmp/smartcar-syntax.mpy "$demo"
done
make -C "$repo/ports/stm32" BOARD=WEACTSTUDIO_MINI_STM32H743 BUILD=build-smartcar-WEACTSTUDIO_MINI_STM32H743 -j4 > "$src/smartcar-stm32-build.log" 2>&1
build="$repo/ports/stm32/build-smartcar-WEACTSTUDIO_MINI_STM32H743"
out="$src/artifacts/smartcar-WEACTSTUDIO_MINI_STM32H743"
mkdir -p "$out"
for ext in hex bin dfu elf; do
    cp "$build/firmware.$ext" "$out/firmware.$ext"
done
arm-none-eabi-nm "$build/firmware.elf" | grep -E 'smartcar_module|smartcar_irq|sc_adc_capture|sc_encoder_capture' > "$out/symbols.txt"
(cd "$out" && sha256sum firmware.* > SHA256SUMS)
tail -12 "$src/smartcar-stm32-build.log"
cat "$out/symbols.txt"
echo 'Firmware exported to Windows artifacts/smartcar-WEACTSTUDIO_MINI_STM32H743'
