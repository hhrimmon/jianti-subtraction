# -*- coding: utf-8 -*-
"""Capture timestamped UART proof that real confirm entry stays on page 4.

Sequence: freeze 0 -> page 2 (debug page command that calls jt_go_finder and
starts the real camera) -> wait for `stream=1 frame=1` -> confirm -> repeated
read-only status queries over >10 seconds. `page 4` is intentionally not used
because its debug handler sets `s_jt_process_frozen=true`, so it cannot prove
the real no-fake-jump behaviour.

The generated log is self-checking: it must contain the real confirm entry
line, an initial `screen=4`, and a final `screen=4` after >=10 seconds.
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
    parser.add_argument("--finder-timeout", type=float, default=25.0)
    parser.add_argument("output")
    args = parser.parse_args()

    start = time.monotonic()
    boot_wait = 4.0
    with serial.Serial(args.port, args.baud, timeout=0.2) as ser, \
            open(args.output, "w", encoding="utf-8", newline="\n") as out:
        def consume(seconds=0.8):
            """Read and log UART output for a short window; return raw bytes."""
            tail = b""
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                chunk = ser.read(256)
                if not chunk:
                    continue
                tail += chunk
                for raw in chunk.splitlines():
                    line = raw.decode("utf-8", "replace").strip()
                    if line:
                        emit("UART_RX " + line)
            return tail

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
        consume(0.5)
        # `page 2` is the debug CLI form that routes through jt_page_cmd();
        # for HOME it calls jt_go_finder(), which runs mipi_cam_init().
        for cmd in (b"freeze 0\r", b"page 2\r"):
            ser.write(cmd)
            ser.flush()
            emit("UART_TX " + cmd.decode("ascii").strip())
            time.sleep(0.5)
            consume(1.0)

        # Wait until the camera stream and the first preview frame are ready;
        # shutter/confirm are intentionally gated on both in the firmware.
        finder_deadline = time.monotonic() + args.finder_timeout
        finder_buffer = b""
        finder_ready = False
        while time.monotonic() < finder_deadline:
            ser.write(b"status\r")
            ser.flush()
            emit("UART_TX finder readiness status")
            finder_buffer += consume(0.8)
            if b"stream=1 frame=1" in finder_buffer and b"screen=2" in finder_buffer:
                finder_ready = True
            if finder_ready:
                break
            finder_buffer = finder_buffer[-4096:]
            time.sleep(0.4)
        if not finder_ready:
            emit("FINDER_READY_TIMEOUT")
            emit("CAPTURE_FAILED reason=finder_not_ready")
            emit("CAPTURE_END")
            raise SystemExit(2)
        emit("FINDER_READY_CONFIRMED " + finder_buffer.decode("utf-8", "replace").strip())

        ser.write(b"status\r")
        ser.flush()
        emit("UART_TX pre-confirm status")
        time.sleep(0.5)
        # `shutter` is the same real handler as the finder shutter button and
        # emits "finder shutter -> process" on success. `confirm` is the send
        # button and produces an equivalent process entry; either is valid.
        ser.write(b"shutter\r")
        ser.flush()
        emit("UART_TX shutter")

        enter_time = time.monotonic()
        emit("ENTER_PROCESS_SENT")
        buffer = b""
        lines = []
        process_entered = False
        first_process_at = None
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
                        lines.append(line)
                        if ("finder shutter -> process" in line or
                                "finder confirm -> process" in line):
                            process_entered = True
                        if first_process_at is None and "screen=4" in line:
                            first_process_at = time.monotonic()
            now = time.monotonic()
            if now >= next_poll:
                ser.write(b"status\r")
                ser.flush()
                emit("UART_TX poll status (read-only)")
                next_poll = now + args.interval
        if buffer.strip():
            line = buffer.decode("utf-8", "replace").strip()
            emit("UART_RX " + line)
            lines.append(line)
            if ("finder shutter -> process" in line or
                    "finder confirm -> process" in line):
                process_entered = True
            if first_process_at is None and "screen=4" in line:
                first_process_at = time.monotonic()

        held_seconds = 0.0
        if first_process_at is not None:
            held_seconds = time.monotonic() - first_process_at
        last_process_status = None
        for line in reversed(lines):
            if "screen=" in line:
                last_process_status = line
                break
        ok = (process_entered and first_process_at is not None and
              held_seconds >= 10.0 and last_process_status is not None and
              "screen=4" in last_process_status and "frozen=0" in last_process_status)
        emit("VALIDATION process_entered=%s held_seconds=%.2f last_status=%s ok=%s" % (
            process_entered, held_seconds, last_process_status, ok))
        emit("CAPTURE_END status=%s" % ("PASS" if ok else "FAIL"))
        if not ok:
            raise SystemExit(3)


if __name__ == "__main__":
    main()
