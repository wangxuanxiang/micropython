"""Sync only smartcar task files into the user's Linux checkout, with backups."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

source = Path('/mnt/d/PROJECTs/micropython')
target = Path('/home/wxx33/micropython')
state_dir = target / '.smartcar-sync'
state_dir.mkdir(exist_ok=True)
state_file = state_dir / 'hashes.json'
state = json.loads(state_file.read_text()) if state_file.exists() else {}
files = [
    'ports/stm32/adc.c', 'ports/stm32/machine_adc.c', 'ports/stm32/main.c',
    'ports/stm32/timer.c', 'ports/stm32/stm32_it.c', 'ports/stm32/machine_pwm.c', 'ports/stm32/spi.c', 'ports/stm32/pyb_spi.c', 'ports/stm32/spi.h',
    'ports/stm32/boards/WEACTSTUDIO_MINI_STM32H743/mpconfigboard.mk',
]
for directory in ('modules/smartcar', 'modules/seekfree', 'tests/smartcar', 'tests/seekfree', 'seekfree_demos/stm32'):
    files.extend(str(p.relative_to(source)) for p in (source / directory).rglob('*') if p.is_file())
def digest(data):
    return hashlib.sha256(data.replace(b'\r\n', b'\n')).hexdigest()
for name in files:
    dest = target / name
    if not dest.exists():
        continue
    old = dest.read_bytes()
    if digest(old) == digest((source / name).read_bytes()) or digest(old) == state.get(name):
        continue
    baseline = subprocess.run(['git', '-C', str(target), 'show', 'HEAD:' + name], capture_output=True)
    if baseline.returncode or digest(old) != digest(baseline.stdout):
        raise SystemExit('Destination has independent changes, not overwritten: ' + name)
for name in files:
    dest = target / name
    backup = state_dir / 'original' / name
    if dest.exists() and not backup.exists():
        backup.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(dest, backup)
    dest.parent.mkdir(parents=True, exist_ok=True)
    data = (source / name).read_bytes().replace(b'\r\n', b'\n')
    dest.write_bytes(data)
    state[name] = digest(data)
state_file.write_text(json.dumps(state, indent=2))
print('Synced', len(files), 'task files; original files backed up in', state_dir)
