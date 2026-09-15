"""Connect A0/A1 to analog signals within 0..3.3 V; common GND."""
from smartcar import ADC_Group, ticker
import time

adc = ADC_Group(1)
pit = ticker(1)

def callback(t):
    # Acquisition already completed in C. Main loop prints the cached values.
    pass

try:
    adc.init(1, period=ADC_Group.PMODE3, average=ADC_Group.AVG16)
    adc.addch('A0')
    adc.addch('A1')
    pit.capture_list(adc)
    pit.callback(callback)
    pit.start(10)
    while True:
        print(pit.ticks(), adc.get())
        time.sleep_ms(100)
finally:
    pit.stop()
    pit.capture_list()
    pit.callback(None)
    adc.deinit()
