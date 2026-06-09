#!/usr/bin/env bash
# Build the Elron display firmware (NCS v2.7.0, board xiao_ble) and copy the
# resulting UF2 into the Windows Downloads folder for drag-drop flashing.
set -euo pipefail

NCS="$HOME/ncs/v2.7.0"
APP="$HOME/nrf_elron"
OUT_NAME="elron_train.uf2"

# Windows-side paths, auto-detected from the active Windows user's profile so this
# isn't tied to one machine. Override with ELRON_WIN_DOWNLOADS / ELRON_WIN_COMPANION.
WIN_HOME="$(wslpath -u "$(powershell.exe -NoProfile -NonInteractive \
	-Command '$Env:USERPROFILE' 2>/dev/null | tr -d '\r\n')" 2>/dev/null || true)"
DOWNLOADS="${ELRON_WIN_DOWNLOADS:-$WIN_HOME/Downloads}"

export PATH="$HOME/.local/bin:$PATH"

# Pull our own flags out of the arg list before handing the rest to west build.
FLASH=1
WEST_ARGS=()
for a in "$@"; do
	if [[ "$a" == "--no-flash" ]]; then FLASH=0; else WEST_ARGS+=("$a"); fi
done

# Build inside the NCS toolchain bundle (cmake/ninja/arm-gcc), but with system
# git first on PATH (the bundle's git is broken on Ubuntu 24.04: libunistring).
nrfutil toolchain-manager launch --ncs-version v2.7.0 -- bash -c '
  export PATH="/usr/bin:$PATH"
  cd "'"$NCS"'"
  west build -b xiao_ble --build-dir "'"$APP"'/build" "'"$APP"'" "$@"
' -- "${WEST_ARGS[@]}"

cp "$APP/build/zephyr/zephyr.uf2" "$DOWNLOADS/$OUT_NAME"
echo "==> $(du -h "$APP/build/zephyr/zephyr.uf2" | cut -f1)  copied to $DOWNLOADS/$OUT_NAME"

# Auto-flash from Windows unless --no-flash was passed. Fully hands-free:
#   1. ask the running app (over BLE) to reboot into the UF2 bootloader
#   2. wait for the bootloader drive and copy the uf2 onto it
# If BLE isn't available (e.g. first bootstrap), flash.ps1 falls back to asking
# for a physical double-tap. Best-effort — never fails the build.
COMPANION_WIN="$(wslpath -w "${ELRON_WIN_COMPANION:-$WIN_HOME/elron_companion}")"
FLASH_PS1_WIN="$(wslpath -w "$DOWNLOADS/elron_flash.ps1")"
if [[ "$FLASH" == "1" ]]; then
	cp "$APP/flash.ps1" "$DOWNLOADS/elron_flash.ps1"
	echo "==> rebooting board into bootloader over BLE..."
	powershell.exe -NoProfile -Command \
		"cd '$COMPANION_WIN'; uv run elron_push.py --bootloader" \
		2>/dev/null || echo "(BLE reboot skipped — board may already be in bootloader)"
	echo "==> flashing..."
	powershell.exe -NoProfile -ExecutionPolicy Bypass \
		-File "$FLASH_PS1_WIN" || \
		echo "(auto-flash needs a double-tap — run flash.ps1, or drag $OUT_NAME)"
fi
