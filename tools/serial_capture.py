"""短时串口抓取：用于复现手机端保存失败时定位设备侧日志。

用法: python tools/serial_capture.py COM14 240 build-manual/serial_capture.log
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM14"
seconds = int(sys.argv[2]) if len(sys.argv) > 2 else 180
path = sys.argv[3] if len(sys.argv) > 3 else "build-manual/serial_capture.log"

ser = serial.Serial(port, 115200, timeout=0.5)
end = time.time() + seconds
with open(path, "w", encoding="utf-8", errors="replace") as fh:
    fh.write(f"# capture {port} {seconds}s\n")
    fh.flush()
    while time.time() < end:
        try:
            data = ser.read(4096)
        except Exception as exc:  # 串口偶尔会被其他程序抢占
            fh.write(f"\n# read error: {exc}\n")
            fh.flush()
            time.sleep(0.5)
            continue
        if data:
            text = data.decode("utf-8", "replace")
            fh.write(text)
            fh.flush()

ser.close()
print(f"captured {path}")
