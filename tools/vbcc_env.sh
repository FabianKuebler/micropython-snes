# Source this to get vbcc on PATH for the SNES port.
#   . tools/vbcc_env.sh
export VBCC="$PWD/vbcc-toolchain/vbcc65816/vbcc65816_linux/vbcc"
export PATH="$VBCC/bin:$PATH"
# vc reads config files from $VBCC/config by default.
vbcc_cc() {
  # vbcc_cc <srcfile> <outfile> [extra args...]
  local src="$1"; shift
  local out="$1"; shift
  vc +snes-hi -c99 -c -O1 \
     -Ivbcc_spike/shim -I. -Imicropython -Iport -Ibuild/mpy \
     "$src" -o "$out" "$@"
}
