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

}  // namespace amd::smi

#endif  // INCLUDE_ROCM_SMI_ROCM_SMI_DXG_H_
