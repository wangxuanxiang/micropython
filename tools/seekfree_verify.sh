#!/bin/bash
set -euo pipefail
src=/mnt/d/PROJECTs/micropython
repo=/home/wxx33/micropython
python3 "$src/tools/smartcar_sync.py"
out="$src/artifacts/seekfree-WEACTSTUDIO_MINI_STM32H743"
mkdir -p "$out"
date -Iseconds > "$out/verification.txt"
make -C "$repo/ports/unix" BUILD=build-seekfree USER_C_MODULES="../../modules/smartcar ../../modules/seekfree" -j4 > "$src/seekfree-unix-build.log" 2>&1
for test in "$repo/tests/seekfree/imu660.py" "$repo/tests/smartcar/ticker.py" "$repo/tests/smartcar/encoder.py" "$repo/tests/smartcar/adc.py"; do
    "$repo/ports/unix/build-seekfree/micropython" "$test" | tee -a "$out/verification.txt"
done
"$repo/mpy-cross/build/mpy-cross" -o /tmp/seekfree-E24.mpy "$repo/seekfree_demos/stm32/E24_imu660rx_demo.py"
"$repo/mpy-cross/build/mpy-cross" -o /tmp/seekfree-resources.mpy "$repo/tests/seekfree/hardware_resources.py"
make -C "$repo/ports/stm32" BOARD=WEACTSTUDIO_MINI_STM32H743 submodules > "$src/seekfree-submodules.log" 2>&1
make -C "$repo/ports/stm32" BOARD=WEACTSTUDIO_MINI_STM32H743 BUILD=build-seekfree-WEACTSTUDIO_MINI_STM32H743 -j4 > "$src/seekfree-stm32-build.log" 2>&1
build="$repo/ports/stm32/build-seekfree-WEACTSTUDIO_MINI_STM32H743"
for ext in hex bin dfu elf; do cp "$build/firmware.$ext" "$out/firmware.$ext"; done
arm-none-eabi-nm "$build/firmware.elf" | grep -E 'seekfree_module|seekfree_sensor_capture|imu_port_capture|smartcar_module' > "$out/symbols.txt"
(cd "$out" && sha256sum firmware.* > SHA256SUMS)
tail -12 "$src/seekfree-stm32-build.log" | tee -a "$out/verification.txt"
cat "$out/symbols.txt"
