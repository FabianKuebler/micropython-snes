#!/usr/bin/env bash
# Run a mailbox-protocol ROM under Mesen headless and print exit code + stdout.
#   tools/vbcc_run.sh <rom> [max_frames]
set -u
ROM="$1"; MAXF="${2:-1800}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MESEN="${MESEN_PATH:-$HOME/bin/Mesen}"

# One-time: copy Mesen config, enable Lua io/os + timeout.
CFG=/tmp/mesencfg
if [ ! -f "$CFG/Mesen2/settings.json" ]; then
  mkdir -p "$CFG"; cp -r "$HOME/.config/Mesen2" "$CFG/"
  python3 - "$CFG/Mesen2/settings.json" <<'EOF'
import json,sys
p=sys.argv[1]; s=json.load(open(p,encoding='utf-8-sig'))
s['Debug']['ScriptWindow']['AllowIoOsAccess']=True
s['Debug']['ScriptWindow']['ScriptTimeout']=120
json.dump(s,open(p,'w',encoding='utf-8-sig'),indent=2)
EOF
fi

LOG=$(mktemp); SCRIPT=$(mktemp --suffix=.lua)
sed -e "s#@LOGFILE@#$LOG#" -e "s#@MAXFRAMES@#$MAXF#" \
    "$ROOT/tests/mailbox_harness.lua.in" > "$SCRIPT"
env XDG_CONFIG_HOME="$CFG" DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 SDL_VIDEODRIVER=dummy \
    "$MESEN" --testrunner "$ROM" "$SCRIPT" >/dev/null 2>&1
code=$?
echo "EXIT=$code"
echo "----- mailbox stdout -----"
cat "$LOG"
echo "--------------------------"
rm -f "$LOG" "$SCRIPT"
