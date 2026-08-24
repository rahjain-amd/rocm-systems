# amd-smi on WSL2 (ROCm develop) — build & run

Reproduces the verified WSL2 build of this branch (`users/rahjain/amd-smi-wsl-develop`,
amd-smi commits on top of `upstream/develop`). AMD Radeon RX 7900 XTX, WSL2.

This build links the **system** netlink stack (libnl-3, libnl-genl-3, libmnl) via
the system `pkg-config`. It has **no** dependence on the bundled ROCm SDK
`rocm_sysdeps` netlink and **does not reference `~/.gfx1250` anywhere**.

## Prerequisites

- System netlink dev packages visible to `pkg-config`:

```bash
sudo apt install libnl-3-dev libnl-genl-3-dev libmnl-dev
# verify:
pkg-config --exists libnl-3.0 && pkg-config --exists libnl-genl-3.0 && pkg-config --exists libmnl && echo ok
```

- The **CORRECT-wkmi** DXG thunk (`librocdxg`) built and installed at:
  `/home/atitest/rocr-develop-correct/install/lib`
  (This thunk links only libc/libstdc++ — it does **not** pull in any netlink, so
  it too is independent of `~/.gfx1250`.)

> ⚠️ **FOOT-GUN — never link the old spike build.** A stale thunk built against the
> WRONG wkmi reports a bogus **1250 MHz** GFX max-clock. That prefix has been renamed
> to `/home/atitest/rocr-develop-build_WRONG_WKMI_DO_NOT_USE`. Do **not** put it on
> `LD_LIBRARY_PATH`. The only correct thunk dir is `rocr-develop-correct/install/lib`.
> A healthy setup shows GFX **MAX_CLK = 2304 MHz**.

## Build

No `PKG_CONFIG_PATH` / `LIBRARY_PATH` overrides — the system `pkg-config` supplies
netlink.

```bash
cd projects/amdsmi
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_CLI=ON \
  -DCMAKE_INSTALL_PREFIX=/home/atitest/amdsmi-nogfx/install
cmake --build build -j"$(nproc)"
cmake --install build
```

cmake should report the system libs, e.g. `libnl-3 3.7.0` and `libmnl 1.0.5` from
`/usr/lib/x86_64-linux-gnu`. `ldd` on the resulting `libamd_smi.so` must show
`libnl-3.so.200 => /lib/x86_64-linux-gnu/...` (system), **not** any
`librocm_sysdeps_nl_*`.

## Run (REQUIRED LD_LIBRARY_PATH)

Only two entries: the CORRECT-wkmi thunk dir and the WSL DirectX libs. Netlink is
resolved from the default loader path (system libnl) — **no `~/.gfx1250`**.

```bash
export LD_LIBRARY_PATH=/home/atitest/rocr-develop-correct/install/lib:/usr/lib/wsl/lib

/home/atitest/amdsmi-nogfx/install/bin/amd-smi list
/home/atitest/amdsmi-nogfx/install/bin/amd-smi metric
```

Order matters: the CORRECT-wkmi thunk dir MUST come first so it wins over any stale copy.

## Sanity check

- `amd-smi list` enumerates GPU 0 (RX 7900 XTX, BDF `0000:03:00.0`).
- `amd-smi metric` idle **GFX_0** shows `MAX_CLK: 2304 MHz` (NOT 1250). If you see
  1250 on GFX, you linked the wrong thunk — fix `LD_LIBRARY_PATH`.
  (Note: `MEM_0 MAX_CLK: 1250 MHz` is the memory clock and is expected/correct.)
- Do not run `amd-smi set`/`reset` for a read-only sanity check.
