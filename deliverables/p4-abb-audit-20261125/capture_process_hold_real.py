# -*- coding: utf-8 -*-
"""Capture timestamped UART proof that real confirm entry stays on page 4.

Sequence: freeze 0 -> page 3 -> confirm -> repeated read-only screen/status
queries over >10 seconds. `page 4` is intentionally not used because it sets
the debug freeze flag and would not prove real no-fake-jump behaviour.
"""

import argparse
import datetime
import time

import serial


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM4")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--duration", type=float, default=12.0)
    parser.add_argument("--interval", type=float, default=2.0)
    parser.add_argument("output")
    args = parser.parse_args()

    start = time.monotonic()
    boot_wait = 4.0
    with serial.Serial(args.port, args.baud, timeout=0.2) as ser, \
            open(args.output, "w", encoding="utf-8", newline="\n") as out:
        def emit(line):
            stamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
            elapsed = time.monotonic() - start
            record = "[%s t=+%06.2fs] %s" % (stamp, elapsed, line)
            out.write(record + "\n")
            out.flush()
            print(record)

        time.sleep(boot_wait)
        ser.reset_input_buffer()
        emit("CAPTURE_START port=%s baud=%d duration=%.1fs interval=%.1fs" % (
            args.port, args.baud, args.duration, args.interval))
        # Commands sent before LVGL init completes can assert in
        # lvgl_port_lock(). When this script starts after boot, nudge the
        # console with a read-only status query and wait for its response.
        ready_deadline = time.monotonic() + 12.0
        ready_buffer = b""
        ready = False
        while time.monotonic() < ready_deadline:
            ser.write(b"status\r")
            ser.flush()
            emit("UART_TX readiness status")
            probe_deadline = time.monotonic() + 1.0
            while time.monotonic() < probe_deadline:
                chunk = ser.read(256)
                if chunk:
                    ready_buffer += chunk
                    if b"screen=" in ready_buffer:
                        ready = True
                        break
            if ready:
                break
            time.sleep(0.2)
        if not ready:
            emit("READY_TIMEOUT")
            emit("CAPTURE_END")
            return
        emit("READY_CONFIRMED " + ready_buffer.decode("utf-8", "replace").strip())
        for cmd in (b"freeze 0\r", b"shutter\r"):
            ser.write(cmd)
            ser.flush()
            emit("UART_TX " + cmd.decode("ascii").strip())
            time.sleep(6.0 if cmd == b"shutter\r" else 0.5)

        ser.write(b"status\r")
        ser.flush()
        emit("UART_TX pre-confirm status")
        time.sleep(0.5)
        ser.write(b"confirm\r")
        ser.flush()
        emit("UART_TX confirm")

        enter_time = time.monotonic()
        emit("ENTER_PROCESS_SENT")
        buffer = b""
        next_poll = enter_time + args.interval
        deadline = enter_time + args.duration
        while time.monotonic() < deadline:
            chunk = ser.read(256)
            if chunk:
                buffer += chunk
                while b"\n" in buffer:
                    raw, buffer = buffer.split(b"\n", 1)
                    line = raw.decode("utf-8", "replace").strip()
                    if line:
                        emit("UART_RX " + line)
            now = time.monotonic()
            if now >= next_poll:
                ser.write(b"screen\r")
                ser.flush()
                emit("UART_TX poll screen (read-only)")
                next_poll = now + args.interval
        if buffer.strip():
            emit("UART_RX " + buffer.decode("utf-8", "replace").strip())
        emit("CAPTURE_END")


if __name__ == "__main__":
    main()
