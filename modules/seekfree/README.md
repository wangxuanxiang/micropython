# seekfree.IMU660RX

`IMU660RX(capture_div=1, *, spi=2, cs='B12')` supports the BMI270 based RA
module and the compatible RB/RC six axis variants. SPI2 uses B13/B14/B15 for
SCK/MISO/MOSI; SPI4 is available when its board pins are free. CS is a GPIO
name and the sensor requires 3.3 V and common ground.

`capture()` updates at the configured divisor, `read()` forces one physical
sample without changing that divisor counter, and `get()` returns one stable
mutable six integer list `[acc_x, acc_y, acc_z, gyro_x, gyro_y, gyro_z]`.
Copy it with `list(imu.get())` before retaining history; do not resize it while
the object is attached to a ticker. `scales()` reports LSB per g and LSB per
degree/second. `info()` and `help()` describe the active configuration.

An IMU can be attached to `smartcar.ticker.capture_list()`. Sampling runs in
the native timer path and Python callbacks observe the latest complete frame.
Only one IMU object is allowed at a time and its SPI bus is reserved until
`deinit()`; detach it from every ticker first. Transport, identity and init
errors are reported as `OSError`; the last complete frame remains available.

The Unix build provides `_imu660_config`, `_imu660_set` and `_imu660_fault` for
deterministic simulation. Unix tests validate API and scheduling semantics;
they cannot validate board wiring, electrical levels, sensor noise or interrupt
latency. This driver does not provide IMU963 fusion, quaternion output or a
data-ready interrupt API.
