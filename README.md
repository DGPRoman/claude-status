# claude-status

A physical Claude Code session-status indicator built on an **ESP32-C3 with a
0.42″ OLED** (a SuperMini-family variant: ESP32-C3, 0.42″ 72×40 OLED, a
**single-color** LED on GPIO8). It sits on your desk over a **USB Type-C** cable
and shows what the session is doing right now — no need to watch the terminal.

```
Claude Code (event) ──hook──> claude-status.sh ──USB-serial──> ESP32 ──> OLED + LED
```

## Quick start (new machine)

The board is already flashed — on a new computer you only set up the host. The
easiest way:

1. **Copy** this folder somewhere on your machine (anywhere, e.g. `~/esp/claude-status`).
2. **Open Claude Code** in that folder.
3. Type **`/setup-device`** (or just: "set up the claude-status device").

Claude installs the dependencies, the udev rule and permissions, writes the hooks
with the correct paths, asks you to plug the board in over USB-C, and verifies it.
Done.

> Without Claude, by hand — one command from the project folder: **`./install.sh`**
> (then `./install.sh --test` to check the display).

## States

The LED is single-color, so status is encoded by **blink pattern** (not color):

| State | Trigger (Claude Code hook)                      | LED                 | OLED      |
|-------|-------------------------------------------------|---------------------|-----------|
| work  | `UserPromptSubmit`, `PreToolUse`, `PostToolUse` | smooth breathing    | `WORK`    |
| perm  | `PermissionRequest`                             | **fast blink**      | `CONFIRM` |
| idle  | `Notification` ("waiting for input")\*          | off                 | `READY`   |
| done  | `Stop`                                          | off                 | `DONE`    |
| start | `SessionStart`                                  | steady medium glow  | `START`   |
| end   | `SessionEnd`                                    | off                 | `IDLE`    |

`CONFIRM` is driven by the **`PermissionRequest`** hook: it fires instantly
(0–2 ms) right before the permission dialog is shown and **works both in the
terminal and in the native VSCode extension** (verified empirically via
`events.log`). **`PostToolUse`** (also → `work`) is what **clears** CONFIRM: the
moment the approved tool finishes, the state flips `perm→work`. Without that hook
`perm` would linger until the next tool call (Claude Code fires no dedicated event
when a permission is answered).

\* idle→READY goes through the catch-all `Notification` hook (`claude-status.sh
notify` reads `.message`). Mapping: "waiting for input" (or an empty/absent
message) → `idle`; a permission message → ignored (see below); **any other
message → `perm`** as a best-effort attention fallback. This hook **does NOT fire
in the native VSCode extension** — a known bug,
[#59718](https://github.com/anthropics/claude-code/issues/59718) (closed as a
duplicate). The permission `Notification` is deliberately ignored (it arrives with
a ~5–7 s delay) — `PermissionRequest` covers it instantly.

> **In the VSCode extension** everything critical works: WORK / DONE / START /
> **CONFIRM**. Only the cosmetic idle→READY-after-idle does not (the dead
> `Notification`). You do **not** need a terminal for it.

For `CONFIRM` the OLED also blinks a full-screen invert. The **BOOT** button
(GPIO9) acknowledges the alert: the fast blink + screen flash calm down to a
steady dim glow — and **stay** calm. Identical status resends (which the
aggregator emits whenever any session fires a hook) no longer re-trigger the
blink; only a genuinely new state, or *more* sessions entering the state,
re-arms it. So an acknowledged CONFIRM won't start flashing again on its own.

## Multiple sessions at once

All sessions write to one device, but `claude-status.sh` acts as an
**aggregator**: each session's state (keyed by `session_id` from the payload) is
stored separately, and the display shows the **most urgent** state across all live
sessions by priority `perm > work > done > start > idle`. So a `CONFIRM` in one
session is **never clobbered** by `WORK` in another. A corner badge on the OLED
shows the **count** of sessions in that state (for active states, ≥2): e.g. `WORK`
+ `3` = three sessions working, `CONFIRM` + `2` = two waiting for confirmation.

`SessionEnd` removes a session immediately; stale ones (no update for more than
`CLAUDE_STATUS_TTL`, default 3600 s) are pruned automatically. The default is
deliberately generous: a session's file is only refreshed on a hook event, so the
TTL must exceed your longest single tool call (a big build, a long test run) or an
actively-working session could be pruned mid-work. Session state lives in
`$XDG_RUNTIME_DIR/claude-status/` (tmpfs); writes are serialized with `flock`.

## Files

- `src/main.cpp` — firmware (U8g2 for the OLED + LED control via LEDC PWM).
- `platformio.ini` — build config.
- `hooks/claude-status.sh` — the aggregator: reads the event JSON on stdin, reduces all session states, sends `state|count` to the port.
- `claude-hooks.snippet.json` — reference: which hooks get enabled (install.sh merges them for you, substituting paths).
- `udev/99-claude-status.rules` — stable `/dev/claude-status` (MODE 0666).
- `install.sh` — one command: jq + udev + permissions + hooks (plus `--test`, `--hooks`).
- `.claude/commands/setup-device.md` — the `/setup-device` slash command: Claude sets everything up itself.

## Installation

### 1. Firmware (only if you build it yourself — the board ships flashed)

**PlatformIO** (recommended):
```bash
pio run -t upload            # build & flash
pio device monitor           # (optional) watch the log
```
If the board doesn't enter flashing mode automatically: hold **BOOT**, tap
**RST**, release **BOOT**, then `pio run -t upload`.

**Arduino IDE** (alternative): board `ESP32C3 Dev Module`, enable
`USB CDC On Boot: Enabled`, library `U8g2`, copy `src/main.cpp` into a sketch.

### 2. Host — one command (jq + udev + permissions + hooks)
```bash
./install.sh                 # jq + udev rule + permissions + Claude Code hooks
./install.sh --test          # demo state sequence
./install.sh --hooks         # re-merge only the hooks (no sudo)
```
`install.sh` installs `jq`, drops the udev rule (MODE `0666` →
`/dev/claude-status`, so no `dialout` group / re-login dance), marks the script
executable, and **merges the hooks into `~/.claude/settings.json`**, substituting
absolute paths for this checkout's location (the old config is backed up). Nothing
to edit by hand.

### 3. Claude Code hooks
`install.sh` already wrote them into `~/.claude/settings.json` (user scope — they
work in every project), with absolute paths for this checkout; the old config is
backed up. To re-merge only the hooks (no sudo): `./install.sh --hooks`.

Done. The next prompt in any **new** Claude Code session will light the device up.

## Protocol (for manual testing)
One line = one update: `STATE|COUNT` (session count; 1 = no badge). Written
straight to the port, bypassing the aggregator:
```bash
printf 'perm|2\n' > /dev/claude-status   # fast blink + flash, badge "2"
printf 'work|1\n' > /dev/claude-status   # smooth breathing
printf 'idle|1\n' > /dev/claude-status   # off
```

## Board pinout (verified)

| Function                    | Pin       |
|-----------------------------|-----------|
| OLED SDA (I2C)              | GPIO5     |
| OLED SCL (I2C)              | GPIO6     |
| LED (single color, active-LOW) | GPIO8  |
| BOOT / button               | GPIO9     |
| USB (native)                | GPIO18/19 |

The display is SSD1306-compatible, I2C `0x3C`. The U8g2 driver
`U8G2_SSD1306_72X40_ER_F_HW_I2C` adds the +28px X offset itself, so no manual
offsets are needed. LED polarity is set by `LED_ACTIVE_LOW` in `src/main.cpp`.

> ⚠️ This is specifically the **SuperMini** variant (single-color LED on GPIO8).
> The similar 01Space ESP32-C3-0.42LCD board instead has an RGB WS2812 on GPIO2 —
> different firmware.

## Troubleshooting

- **`/dev/claude-status` doesn't appear** — check `lsusb | grep 303a`; it should
  be `303a:1001`. Reinstall the udev rule and replug the cable.
- **`Permission denied` when writing** — the udev rule isn't active; check that
  `/dev/claude-status` exists with `crw-rw-rw-` permissions (reinstall the rule and
  replug the board).
- **LED doesn't react / stays lit** — a constant red is the power indicator (not
  controllable). If states are inverted (DONE dark, READY lit), flip
  `LED_ACTIVE_LOW` in `src/main.cpp`.
- **Board resets on every update** — some hosts toggle DTR when opening the port.
  The script already sets `stty -hupcl`; if that doesn't help, keep the port open
  with a background reader: `cat /dev/claude-status &`.
- **Picture shifted / clipped** — make sure the `..._72X40_...` constructor is
  used, not `128X64`.
- **Port appears then vanishes / can't open it** — on some distros ModemManager
  briefly grabs any new ACM device. Replug and wait a few seconds, or exclude the
  device from ModemManager.
- **No reaction to hooks** — test manually with
  `printf 'work|1\n' > /dev/claude-status`; if that works but Claude doesn't, check
  the hook path in `~/.claude/settings.json` and that `jq` is installed. For a
  trace of what actually reaches the hook, set `CLAUDE_STATUS_DEBUG=1` and watch
  `$XDG_RUNTIME_DIR/claude-status/events.log`.

## Want it wireless?
The firmware can easily move to WiFi (the ESP32 serves an HTTP endpoint + mDNS
`claude-status.local`, and the hook does a `curl`). Say the word and I'll add that
variant.
