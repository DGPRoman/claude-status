---
description: Set up the claude-status ESP32 OLED device on this Ubuntu machine (deps, udev, hooks)
---

You are setting up the **claude-status** ESP32-C3 OLED status device on THIS Ubuntu
machine for the current user. Be concise and act — this is a routine install.

## Ground rules
- The firmware is ALREADY flashed on the board. Do **NOT** build, flash, or touch
  the firmware, and do NOT install PlatformIO. This is host-side setup only.
- The project root is the directory this repo lives in (where `install.sh` is).
  Use `./install.sh` from there — it derives every path from its own location, so
  nothing is hardcoded and it works for any username / install location.
- Never hand-edit `~/.claude/settings.json` if `install.sh` succeeds — it does the
  merge (with a backup) itself.

## Steps
1. Do the setup. There are two kinds of steps: **sudo** (apt + udev) and
   **no-sudo** (the hooks merge). Your Bash tool usually CANNOT type an
   interactive sudo password, so don't let a `sudo` command hang.

   a. First do the part you can always do yourself — the hooks merge (no sudo):
      ```bash
      ./install.sh --hooks
      ```
   b. Then handle the sudo part. Check whether sudo works without a password:
      ```bash
      sudo -n true 2>/dev/null && echo NOPASSWD || echo NEEDS_PASSWORD
      ```
      - If `NOPASSWD`: just run `./install.sh` (it re-does hooks harmlessly and
        adds jq + the udev rule).
      - If `NEEDS_PASSWORD`: do NOT run sudo yourself (it would hang). Ask the user
        to run this once in their own terminal, then tell you when it's done:
        ```bash
        ./install.sh
        ```
        (It installs `jq`, the udev rule → persistent `/dev/claude-status` at MODE
        0666, and re-merges the hooks. Safe to run even though you already did
        `--hooks`.)

2. Ask the user to plug the board in via **USB-C** (if not already), then verify:
   ```bash
   ls -l /dev/claude-status          # symlink -> ttyACMx, perms crw-rw-rw-
   lsusb | grep 303a                 # 303a:1001 present
   jq '.hooks | keys' ~/.claude/settings.json
   ```
   The hooks list must contain all 7 events: Notification, PermissionRequest,
   PostToolUse, PreToolUse, SessionEnd, Stop, UserPromptSubmit.

3. Smoke-test the display (only if the device node exists):
   ```bash
   ./install.sh --test
   ```
   Ask the user to confirm the OLED shows WORK, then WORK with a "3" badge, then
   CONFIRM, then CONFIRM with a "2" badge, then goes dark (off).

4. Report the result. If the port never appears or the test fails, diagnose using
   the **Troubleshooting** section of `README.md` (check `lsusb | grep 303a`, replug
   so the udev rule fires, check ModemManager isn't holding the port).

Note: hooks take effect in new Claude Code sessions. The current session may need
to be reopened for them to be active.
