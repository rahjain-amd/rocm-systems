/*
 * Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef INCLUDE_ROCM_SMI_ROCM_SMI_DXG_H_
#define INCLUDE_ROCM_SMI_ROCM_SMI_DXG_H_

#include <cstdint>
#include <string>
#include <vector>

namespace amd::smi {

// One GPU node as seen through the DXG thunk (librocdxg / hsaKmt*). This is the
// WSL2 analogue of a /sys/class/kfd topology node: the sysfs path is absent on
// WSL so the same data is sourced from the paravirtualized DXG stack instead.
struct DxgNodeInfo {
  uint32_t node_index = 0;      // hsaKmt topology node index (GPU node)
  uint16_t vendor_id = 0;       // PCI vendor id (0x1002 for AMD)
  uint16_t device_id = 0;       // PCI device id
  uint64_t location_id = 0;     // synthesized/real BDF location id (bus/dev/fn)
  uint64_t domain = 0;          // PCI domain
  uint64_t bdfid = 0;           // (domain << 32) | location_id -- rsmi map key
  uint64_t gpu_id = 0;          // synthesized non-zero KFD gpu id (GUID)
  uint32_t simd_count = 0;      // total SIMD count (NumFComputeCores)
  uint32_t drm_render_minor = 0;
  uint64_t local_mem_size = 0;
  std::string name;             // marketing name

  // P1: static clock/VRAM data captured from the same hsaKmt topology query.
  // These are the hardware maxima the DXG topology reports (not live values);
  // live values come from DxgQueryLiveStats() via a direct D3DKMT query.
  uint32_t max_gfx_clk_mhz = 0;  // MaxEngineClockMhzFCompute (max gfx/SCLK)
  uint32_t max_mem_clk_mhz = 0;  // MemoryClockMax of the VRAM bank (max MCLK)
  uint64_t vram_total_bytes = 0; // sum of FRAME_BUFFER heap SizeInBytes

  // Windows adapter LUID (from HsaNodeProperties Luid{Low,High}Part). Used to
  // match this GPU to a D3DKMT adapter for the live statistics query.
  uint32_t luid_low = 0;
  int32_t luid_high = 0;
  bool luid_valid = false;
};

// Live per-GPU statistics sourced from a direct D3DKMT query (libdxcore.so /
// D3DKMTQueryStatistics). These are the values the hsaKmt topology cannot give:
// current VRAM residency and engine utilization. Fields carry an explicit valid
// flag so callers can cleanly report N/A when a value could not be sampled.
struct DxgLiveStats {
  uint64_t vram_used_bytes = 0;   // sum of BytesResident over local (non-aperture) segments
  bool vram_used_valid = false;
  uint32_t gfx_activity_pct = 0;  // busiest engine node's running-time delta over the sample window
  bool gfx_activity_valid = false;
};

// Returns true when running under WSL2, where /dev/dxg is present and the KFD
// sysfs topology is absent. Used to gate all DXG-sourced enumeration so native
// Linux behavior is completely unchanged.
bool is_wsl();

// Enumerates AMD GPU nodes via the DXG thunk (librocdxg) using the hsaKmt* API
// (hsaKmtOpenKFD -> hsaKmtAcquireSystemProperties -> hsaKmtGetNodeProperties),
// mirroring the proven call sequence in the dxg_sanity harness. librocdxg is
// loaded via dlopen at runtime, so native Linux builds carry no hard link
// dependency on it. Returns true on success (out populated, possibly empty).
bool DxgEnumerateGpuNodes(std::vector<DxgNodeInfo>* out);

// Samples live GPU statistics (VRAM used, engine utilization) for the adapter
// matching the given LUID, via libdxcore.so's D3DKMT ABI. When luid_valid is
// false (or no adapter matches the LUID) the first enumerated adapter is used,
// which is correct for the common single-GPU WSL case. Engine utilization is
// derived from D3DKMTQueryStatistics running-time deltas sampled over
// sample_ms milliseconds. Returns true if at least one field was populated;
// individual field validity is carried in DxgLiveStats. Safe to call on native
// Linux (returns false because libdxcore.so is absent).
bool DxgQueryLiveStats(uint32_t luid_low, int32_t luid_high, bool luid_valid,
                       uint32_t sample_ms, DxgLiveStats* out);

}  // namespace amd::smi

#endif  // INCLUDE_ROCM_SMI_ROCM_SMI_DXG_H_
