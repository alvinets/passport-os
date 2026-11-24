"""串口长抓取，支持断线自动重连（用于设备重启 / 热插拔后仍能继续抓日志）。

用法: python tools/serial_watch.py COM14 600 build-manual/serial_watch.log
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM14"
seconds = int(sys.argv[2]) if len(sys.argv) > 2 else 300
path = sys.argv[3] if len(sys.argv) > 3 else "build-manual/serial_watch.log"

end = time.time() + seconds
with open(path, "a", encoding="utf-8", errors="replace") as fh:
    fh.write(f"\n===== watch {port} {seconds}s @ {time.strftime('%H:%M:%S')} =====\n")
    fh.flush()
    while time.time() < end:
        try:
            ser = serial.Serial(port, 115200, timeout=0.5)
        except Exception as exc:
            fh.write(f"\n# open failed: {exc}\n")
            fh.flush()
            time.sleep(1.0)
            continue
        fh.write(f"\n# port opened @ {time.strftime('%H:%M:%S')}\n")
        fh.flush()
        try:
            while time.time() < end:
                try:
                    data = ser.read(4096)
                except Exception as exc:
                    fh.write(f"\n# read error: {exc}\n")
                    fh.flush()
                    break
                if data:
                    fh.write(data.decode("utf-8", "replace"))
                    fh.flush()
        finally:
            try:
                ser.close()
            except Exception:
                pass
        time.sleep(1.0)

print(f"done {path}")
