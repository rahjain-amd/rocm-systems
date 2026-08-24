# amd-smi on WSL2 (ROCm develop) — build & run

Reproduces the verified WSL2 build of this branch (`users/rahjain/amd-smi-wsl-develop`,
9 amd-smi commits on top of `upstream/develop`). AMD Radeon RX 7900 XTX, WSL2.

## Prerequisites

- ROCm SDK devel wheel unpacked at `~/.gfx1250` (provides `rocm_sysdeps` drm/netlink + libs).
- The **CORRECT-wkmi** DXG thunk (`librocdxg`) built and installed at:
  `/home/atitest/rocr-develop-correct/install/lib`

> ⚠️ **FOOT-GUN — never link the old spike build.** A stale thunk built against the
> WRONG wkmi reports a bogus **1250 MHz** GFX max-clock. That prefix has been renamed
> to `/home/atitest/rocr-develop-build_WRONG_WKMI_DO_NOT_USE`. Do **not** put it on
> `LD_LIBRARY_PATH`. The only correct thunk dir is `rocr-develop-correct/install/lib`.
> A healthy setup shows GFX **MAX_CLK = 2304 MHz**.

## Build

```bash
SYSDEPS=~/.gfx1250/lib/python3.12/site-packages/_rocm_sdk_devel/lib/rocm_sysdeps
cd projects/amdsmi
PKG_CONFIG_PATH="$SYSDEPS/lib/pkgconfig" \
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_CLI=ON \
  -DCMAKE_INSTALL_PREFIX=/home/atitest/amdsmi-ours-on-develop/install
cmake --build build -j"$(nproc)"
cmake --install build
```

## Run (REQUIRED LD_LIBRARY_PATH)

```bash
export LD_LIBRARY_PATH=\
/home/atitest/rocr-develop-correct/install/lib:\
$HOME/.gfx1250/lib/python3.12/site-packages/_rocm_sdk_devel/lib/rocm_sysdeps/lib:\
/usr/lib/wsl/lib

/home/atitest/amdsmi-ours-on-develop/install/bin/amd-smi list
/home/atitest/amdsmi-ours-on-develop/install/bin/amd-smi metric
```

Order matters: the CORRECT-wkmi thunk dir MUST come first so it wins over any stale copy.

## Sanity check

- `amd-smi list` enumerates GPU 0 (RX 7900 XTX, BDF `0000:03:00.0`).
- `amd-smi metric` idle GFX shows `MAX_CLK: 2304 MHz` (NOT 1250). If you see 1250,
  you linked the wrong thunk — fix `LD_LIBRARY_PATH`.
- Do not run `amd-smi set`/`reset` for a read-only sanity check.
