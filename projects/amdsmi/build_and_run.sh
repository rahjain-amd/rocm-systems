#!/usr/bin/env bash
#
# build_and_run.sh — build amd-smi (WSL2 / DXG, ROCm develop branch) and run it.
#
# Branch: users/rahjain/amd-smi-wsl-develop (amd-smi commits on ROCm develop).
# Target: AMD GPU on WSL2 via /dev/dxg. Native Linux is unaffected (WSL is
# detected at runtime; all WSL code is guarded by is_wsl()).
#
# Netlink: this build links the SYSTEM netlink stack (libnl-3, libnl-genl-3,
# libmnl) discovered via the system pkg-config. It has NO dependence on the
# bundled ROCm SDK "rocm_sysdeps" netlink and does not reference ~/.gfx1250
# anywhere. Install the dev packages once if pkg-config can't find them:
#   sudo apt install libnl-3-dev libnl-genl-3-dev libmnl-dev
#
# Usage:
#   ./build_and_run.sh            # build, then run a read-only sanity pass
#   ./build_and_run.sh --build    # build only
#   ./build_and_run.sh --run      # run only (skip build)
#   ./build_and_run.sh --thunk    # also (re)build the DXG thunk librocdxg first
#
set -euo pipefail

# ---------------------------------------------------------------------------
# Paths (override via env if your layout differs)
# ---------------------------------------------------------------------------
AMDSMI_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"          # projects/amdsmi
REPO_ROOT="$(cd "$AMDSMI_DIR/../.." && pwd)"                        # rocm-systems root

AMDSMI_INSTALL="${AMDSMI_INSTALL:-/home/atitest/amdsmi-nogfx/install}"

# CORRECT-wkmi DXG thunk. NEVER point this at the old spike build
# (/home/atitest/rocr-develop-build_WRONG_WKMI_DO_NOT_USE) — it reports a bogus
# 1250 MHz GFX max clock. The healthy thunk shows GFX MAX_CLK = 2304 MHz.
ROCDXG_INSTALL="${ROCDXG_INSTALL:-/home/atitest/rocr-develop-correct/install}"

# WSL DirectX shim libs (libdxcore.so) live here.
WSL_LIB="${WSL_LIB:-/usr/lib/wsl/lib}"

# Windows SDK 'shared' headers needed only when (re)building the DXG thunk.
WIN_SDK="${WIN_SDK:-/mnt/c/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/shared}"

DO_BUILD=1; DO_RUN=1; DO_THUNK=0
case "${1:-}" in
  --build) DO_RUN=0 ;;
  --run)   DO_BUILD=0 ;;
  --thunk) DO_THUNK=1 ;;
  "")      ;;
  *) echo "usage: $0 [--build|--run|--thunk]"; exit 2 ;;
esac

echo "== amd-smi WSL build_and_run =="
echo "   repo:     $REPO_ROOT"
echo "   thunk:    $ROCDXG_INSTALL/lib"
echo "   install:  $AMDSMI_INSTALL"

# ---------------------------------------------------------------------------
# Sanity: system netlink dev must be visible to pkg-config
# ---------------------------------------------------------------------------
if [ "$DO_BUILD" = 1 ] || [ "$DO_THUNK" = 1 ]; then
  for m in libnl-3.0 libnl-genl-3.0 libmnl; do
    pkg-config --exists "$m" || {
      echo "ERROR: system pkg-config can't find '$m'."
      echo "       Install it: sudo apt install libnl-3-dev libnl-genl-3-dev libmnl-dev"
      exit 1
    }
  done
  echo "   netlink:  system libnl-3 $(pkg-config --modversion libnl-3.0) (pkg-config)"
fi

# ---------------------------------------------------------------------------
# Optional: (re)build the DXG thunk librocdxg (against system libs)
# ---------------------------------------------------------------------------
if [ "$DO_THUNK" = 1 ]; then
  echo "== building DXG thunk (librocdxg) =="
  [ -d "$WIN_SDK" ] || { echo "ERROR: WIN_SDK not found: $WIN_SDK"; exit 1; }
  HSAKMT_DIR="$REPO_ROOT/projects/rocr-runtime/libhsakmt"
  cmake -S "$HSAKMT_DIR" -B "$HSAKMT_DIR/build-dxg" -G Ninja \
    -DWIN_SDK="$WIN_SDK" \
    -DBUILD_ROCDXG_TESTS=ON \
    -DCMAKE_INSTALL_PREFIX="$ROCDXG_INSTALL"
  cmake --build "$HSAKMT_DIR/build-dxg" -j"$(nproc)"
  cmake --install "$HSAKMT_DIR/build-dxg"
fi

# ---------------------------------------------------------------------------
# Build amd-smi (system pkg-config; no PKG_CONFIG_PATH/LIBRARY_PATH overrides)
# ---------------------------------------------------------------------------
if [ "$DO_BUILD" = 1 ]; then
  echo "== building amd-smi =="
  cmake -S "$AMDSMI_DIR" -B "$AMDSMI_DIR/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_CLI=ON \
    -DCMAKE_INSTALL_PREFIX="$AMDSMI_INSTALL"
  cmake --build "$AMDSMI_DIR/build" -j"$(nproc)"
  cmake --install "$AMDSMI_DIR/build"
  echo "== build complete: $AMDSMI_INSTALL/bin/amd-smi =="
fi

# ---------------------------------------------------------------------------
# Run (read-only sanity pass)
# ---------------------------------------------------------------------------
if [ "$DO_RUN" = 1 ]; then
  # CORRECT-wkmi thunk MUST come first so it wins over any stale copy.
  # Only the thunk dir + the WSL DirectX libs are needed — netlink resolves
  # to the system libnl in the default loader path (no ~/.gfx1250).
  export LD_LIBRARY_PATH="$ROCDXG_INSTALL/lib:$WSL_LIB"
  BIN="$AMDSMI_INSTALL/bin/amd-smi"
  [ -x "$BIN" ] || { echo "ERROR: amd-smi not built at $BIN (run with --build first)"; exit 1; }

  echo; echo "== amd-smi list =="   ; "$BIN" list
  echo; echo "== amd-smi static ==" ; "$BIN" static
  echo; echo "== amd-smi metric ==" ; "$BIN" metric
  echo; echo "== amd-smi monitor ==" ; "$BIN" monitor
  echo; echo "== amd-smi process ==" ; "$BIN" process

  echo
  echo "Sanity: 'amd-smi list' should show GPU 0 (BDF 0000:03:00.0);"
  echo "        'amd-smi metric' idle GFX should show MAX_CLK: 2304 MHz (not 1250)."
  echo "        (Read-only: this script never runs 'amd-smi set' or 'reset'.)"
fi
