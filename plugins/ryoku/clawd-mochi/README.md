# Clawd Mochi

A bar mark for the [Clawd Mochi](https://github.com/yousifamanuel/clawd-mochi) desk toy
(ESP32-C3 + 240×240 screen) running the [Clawd Mochi Plus](https://github.com/GbaGuy/clawd-mochi-plus) firmware.

- **Glyph:** a Clawd mark; click it for the control panel.
- **Panel:** page buttons (‹ prev / auto / next ›, clawd / clock / usage / timer / music / stats / history / weather / calendar / events / notifs), clear notification history, mood buttons, a focus timer (25m focus / 5m break / stop), "say" a message on the screen, backlight on/off, and a link to the
  full controller (with the drawing pad).

## What it runs, reads, writes

- Runs `clawd-mochi page|mood|say|bl|timer …` when you click, and, only while the panel is open, `clawd-mochi state` every 2 s (to highlight the pinned page) and `clawd-mochi snap` every second (a live mirror of the screen, written as PNGs to the plugin state dir).
  array, never through a shell. Nothing runs in the background.
- "controller ↗" runs `clawd-mochi ui` (the `clawd-mochi-ui` localhost bridge on
  127.0.0.1:8484) or, if it is already running, `xdg-open http://127.0.0.1:8484/`.
- Writes only the two mirror PNGs in its state dir. Network: only `127.0.0.1` (its own localhost controller page). No privileged commands.

## Requirements

`clawd-mochi` and `clawd-mochi-ui` on `PATH` (from this repo's `host/` folder), and the
Mochi on USB with the custom firmware.
