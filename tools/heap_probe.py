#!/usr/bin/env python3
"""Read Party Play heap figures over the USB console without resetting the device.

Usage: heap_probe.py <port> <label> <out.json> [--wait SECONDS] [--samples N]
Sends `heap` (and `yk info` when available) and records every HEAP/YK_INFO line.
"""
import json, sys, time
import serial

port, label, out = sys.argv[1:4]
wait = int(sys.argv[sys.argv.index('--wait') + 1]) if '--wait' in sys.argv else 0
samples = int(sys.argv[sys.argv.index('--samples') + 1]) if '--samples' in sys.argv else 3
time.sleep(wait)
records = []
s = serial.Serial()
s.port, s.baudrate, s.timeout = port, 115200, 0.2
s.open()
# Release RTS, then DTR (never DTR low with RTS high, which resets the S3).
s.rts = False
s.dtr = False
try:
    for _ in range(samples):
        for cmd in (b'heap\r\n', b'yk info\r\n'):
            s.reset_input_buffer()
            s.write(cmd)
            deadline = time.time() + 2
            while time.time() < deadline:
                line = s.readline().decode('utf-8', 'replace').strip()
                if line.startswith('HEAP ') or line.startswith('YK_INFO '):
                    records.append({'at': time.strftime('%Y-%m-%dT%H:%M:%S'), 'line': line})
                    break
        time.sleep(5)
finally:
    s.close()
result = {'label': label, 'port': port, 'idleSeconds': wait, 'records': records}
json.dump(result, open(out, 'w'), ensure_ascii=False, indent=2)
print(json.dumps(result, ensure_ascii=False))
