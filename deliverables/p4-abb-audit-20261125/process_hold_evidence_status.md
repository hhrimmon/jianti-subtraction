# Process page 10s evidence status

## Follow-up to ABB review of 95ece6e

The code, build/flash, and manifest checks for `95ece6e` were already accepted.
The only rejected item was the old `p4_process_no_fake_jump2.log.txt`, which was
9 bytes and had no host timestamp, start/end, or polling evidence.

## What was added

- Debug-only read-only UART status command `screen` / `status`.
  It prints the current screen index, process freeze flag, and solve cache
  state. It does not change pages or solve state.
- Build log: `build_process_hold_evidence.log.txt`
  (`0xb18900`, app partition 8% free).
- Flash log: `flash_process_hold_evidence.log.txt` (three regions verified).
- Capture script: `capture_process_hold_real.py`.
- Timestamped captures: `p4_process_hold_10s_real_timestamped*.log.txt`.

## Capture result

The board used for this rerun did **not** enter the real process page because
the camera stream was not ready. The serial logs show:

```text
JT_UI: finder shutter ignored: stream_ready=0 frame_ready=0
JT_UI: finder confirm ignored: stream_ready=0 frame_ready=0
screen=2 frozen=0 solve_job=- solve_result=0 solve_questions=0
```

Screen 2 is the finder page. Because the real shutter/confirm path was gated
by `stream_ready=0 frame_ready=0`, these logs do **not** satisfy ABB's
request and are retained only as truthful failed-attempt evidence.

`page 4` was deliberately not used for this proof: `app_jt_ui_debug_show_page(4)`
sets `s_jt_process_frozen = true`, so a page-4 debug sample would not prove the
real no-fake-jump behavior.

## Required next step

Bring the camera stream up normally on the finder page (real live preview,
then shutter/confirm), and rerun `capture_process_hold_real.py`. The accepted
log must show entry to screen 4 followed by repeated `screen=4 ...` samples
spanning more than 10 seconds.
