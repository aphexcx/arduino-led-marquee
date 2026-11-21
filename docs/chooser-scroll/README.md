# Chooser preview: recordings

Both clips are the real firmware image of this commit running in the sign emulator (`emulator/`, a separate
pull request), with `emulator/fake_app.py --contract 3` as the app at real-time speed. Side A is the upper
panel, side B the lower. Each clip starts with the sign's five seconds of startup words and stars.

- `push-two-changes-mid-scroll.gif`: a 64-character message beside `1/3` holds for 0.6 s and scrolls. After
  4 s the app selects another message and pushes it (the line LOW for 50 ms, 20 ms, the message): the new
  text is up from its start a tenth of a second later. The same again 4 s later. Then the app leaves chooser
  mode (the line LOW and nothing behind it): the sign asks, and shows what it is given (`BYE`).
- `blink-short-preview-then-push.gif`: a message that fits beside the count blinks (a fade to black in a
  tenth of a second, 0.2 s dark, `CHOOSER_BLINK_PAUSE_MS`). After 3 s another short one is pushed, after 3 s
  more the app leaves chooser mode.

The clips recorded for contract 2.1 (the `?` poll and its checksum) are in this folder's history.

A GIF shows a new picture every 20 ms at most; the sign latches a frame of the blink every 2.2 ms and one of
the scroll every 36 to 37 ms.
