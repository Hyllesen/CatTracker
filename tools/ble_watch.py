#!/usr/bin/env python3
"""Watch the handheld's serial output for BLE subscribe/notify diagnostics."""
import serial, sys, time

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem111301"
dur = float(sys.argv[2]) if len(sys.argv) > 2 else 90.0

s = serial.Serial()
s.port = port
s.baudrate = 115200
s.timeout = 0.3
s.dtr = False
s.rts = False
s.open()
time.sleep(2.0)
try:
    s.reset_input_buffer()
except Exception:
    pass

print(f"watching {port} for {dur:.0f}s -- subscribe in nRF Connect now", flush=True)
start = time.time()
while time.time() - start < dur:
    try:
        line = s.readline()
        if not line:
            continue
        t = line.decode("utf-8", errors="replace").rstrip()
        if t.startswith("[ble]") or "subscribe" in t.lower() or "pkt #" in t:
            print(t, flush=True)
    except serial.SerialTimeoutException:
        pass
    except KeyboardInterrupt:
        break
s.close()
