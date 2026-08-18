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

#include "rocm_smi/rocm_smi_dxg.h"

#include <dlfcn.h>
#include <unistd.h>

#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

#include "hsakmt/hsakmttypes.h"
#include "rocm_smi/rocm_smi_logger.h"

namespace amd::smi {

namespace {

// hsaKmt* entry points consumed from librocdxg. Signatures mirror the proven
// dxg_sanity harness so the ABI contract is identical.
using fn_void = HSAKMT_STATUS (*)(void);
using fn_sys = HSAKMT_STATUS (*)(HsaSystemProperties*);
using fn_node = HSAKMT_STATUS (*)(HSAuint32, HsaNodeProperties*);
using fn_abi = HSAKMT_STATUS (*)(HsaStructureSizes*);

// Candidate sonames for the DXG thunk. The unqualified soname is tried first so
// LD_LIBRARY_PATH (or a packaged copy) can override; the explicit build/install
// path is the fallback used on this WSL bring-up environment.
constexpr const char* kLibRocDxgSoname = "librocdxg.so";
constexpr const char* kLibRocDxgSoVer = "librocdxg.so.10";

void* open_librocdxg() {
  void* h = ::dlopen(kLibRocDxgSoname, RTLD_NOW | RTLD_GLOBAL);
  if (!h) {
    h = ::dlopen(kLibRocDxgSoVer, RTLD_NOW | RTLD_GLOBAL);
  }
  return h;
}

}  // namespace

bool is_wsl() {
  // Memoize: the environment does not change over the process lifetime.
  static const bool wsl = []() {
    const bool dxg_present = (::access("/dev/dxg", F_OK) == 0);
    const bool kfd_absent = (::access("/sys/class/kfd", F_OK) != 0);
    if (dxg_present && kfd_absent) {
      return true;
    }
    // Fallback signal: kernel version string carries the WSL marker.
    std::ifstream ver("/proc/version");
    if (ver.is_open()) {
      std::string line;
      std::getline(ver, line);
      if (line.find("microsoft") != std::string::npos ||
          line.find("Microsoft") != std::string::npos) {
        return true;
      }
    }
    return false;
  }();
  return wsl;
}

bool DxgEnumerateGpuNodes(std::vector<DxgNodeInfo>* out) {
  std::ostringstream ss;
  if (out == nullptr) {
    return false;
  }
  out->clear();

  void* h = open_librocdxg();
  if (h == nullptr) {
    ss << __PRETTY_FUNCTION__ << " | dlopen(librocdxg) failed: " << dlerror();
    LOG_ERROR(ss);
    return false;
  }

  auto DxgAbiCheck = reinterpret_cast<fn_abi>(dlsym(h, "DxgAbiCheck"));
  auto openkfd = reinterpret_cast<fn_void>(dlsym(h, "hsaKmtOpenKFD"));
  auto acquire = reinterpret_cast<fn_sys>(dlsym(h, "hsaKmtAcquireSystemProperties"));
  auto getnode = reinterpret_cast<fn_node>(dlsym(h, "hsaKmtGetNodeProperties"));
  auto release = reinterpret_cast<fn_void>(dlsym(h, "hsaKmtReleaseSystemProperties"));

  if (!openkfd || !acquire || !getnode) {
    ss << __PRETTY_FUNCTION__ << " | required hsaKmt symbols missing in librocdxg";
    LOG_ERROR(ss);
    return false;
  }

  // ABI handshake: populate HsaStructureSizes exactly like dxg_sanity so the
  // thunk accepts our struct layout (DxgAbiCheck must return SUCCESS/0).
  if (DxgAbiCheck) {
    HsaStructureSizes sizes;
    memset(&sizes, 0, sizeof(sizes));
    sizes.StructureSizes = static_cast<HSAuint16>(sizeof(HsaStructureSizes));
    sizes.SizeOfHsaNodeProperties = static_cast<HSAuint16>(sizeof(HsaNodeProperties));
    sizes.SizeOfHsaExternalHandleDesc = static_cast<HSAuint16>(sizeof(HsaHandleImportDesc));
    HSAKMT_STATUS a = DxgAbiCheck(&sizes);
    if (a != HSAKMT_STATUS_SUCCESS) {
      ss << __PRETTY_FUNCTION__ << " | DxgAbiCheck failed -> " << a;
      LOG_ERROR(ss);
      // Continue anyway; some thunks tolerate a mismatch. Logged for triage.
    }
  }

  HSAKMT_STATUS s = openkfd();
  if (s != HSAKMT_STATUS_SUCCESS && s != HSAKMT_STATUS_KERNEL_ALREADY_OPENED) {
    ss << __PRETTY_FUNCTION__ << " | hsaKmtOpenKFD failed -> " << s;
    LOG_ERROR(ss);
    return false;
  }

  HsaSystemProperties sys;
  memset(&sys, 0, sizeof(sys));
  s = acquire(&sys);
  if (s != HSAKMT_STATUS_SUCCESS) {
    ss << __PRETTY_FUNCTION__ << " | hsaKmtAcquireSystemProperties failed -> " << s;
    LOG_ERROR(ss);
    return false;
  }

  ss << __PRETTY_FUNCTION__ << " | NumNodes = " << sys.NumNodes;
  LOG_INFO(ss);

  uint32_t gpu_index = 0;
  for (HSAuint32 i = 0; i < sys.NumNodes; ++i) {
    HsaNodeProperties np;
    memset(&np, 0, sizeof(np));
    HSAKMT_STATUS ns = getnode(i, &np);
    if (ns != HSAKMT_STATUS_SUCCESS) {
      ss << __PRETTY_FUNCTION__ << " | node " << i << " GetNodeProperties -> " << ns;
      LOG_INFO(ss);
      continue;
    }

    // Skip pure CPU / latency-only nodes: a GPU node has compute cores and a
    // non-zero device id.
    if (np.NumFComputeCores == 0 && np.DeviceId == 0) {
      continue;
    }

    DxgNodeInfo info;
    info.node_index = i;
    info.vendor_id = np.VendorId;
    info.device_id = np.DeviceId;
    info.simd_count = np.NumFComputeCores;
    info.drm_render_minor = static_cast<uint32_t>(np.DrmRenderMinor);
    info.local_mem_size = np.LocalMemSize;

    // Marketing name is UTF-16; on WSL it is plain ASCII in the low byte, so a
    // narrowing copy is sufficient for display.
    char name[HSA_PUBLIC_NAME_SIZE + 1];
    int j = 0;
    for (; j < HSA_PUBLIC_NAME_SIZE && np.MarketingName[j]; ++j) {
      name[j] = static_cast<char>(np.MarketingName[j]);
    }
    name[j] = '\0';
    info.name = name;

    // Location id / BDF: use the real one from the thunk when present. On WSL
    // the DXG topology frequently reports LocationId == 0, which would collapse
    // multiple GPUs onto one bdfid map key. Synthesize a stable, unique BDF per
    // GPU in that case: bus 0, device = gpu_index (bits [7:3]), function 0.
    if (np.LocationId != 0) {
      info.location_id = np.LocationId;
    } else {
      info.location_id = static_cast<uint64_t>((gpu_index & 0x1F) << 3);
    }
    info.domain = 0;
    info.bdfid = (info.domain << 32) | info.location_id;

    // Synthesize a non-zero, unique KFD gpu id (GUID). rocm_smi treats gpu_id
    // 0 as a CPU node and drops it, and keys kfd_node_map_ on this value.
    info.gpu_id = 0x1000ULL + i;

    out->push_back(info);
    ++gpu_index;

    ss << __PRETTY_FUNCTION__ << " | GPU node " << i << " vendor=0x" << std::hex << np.VendorId
       << " device=0x" << np.DeviceId << std::dec << " simd=" << np.NumFComputeCores
       << " location_id=" << info.location_id << " name='" << info.name << "'";
    LOG_INFO(ss);
  }

  if (release) {
    release();
  }
  // Intentionally do NOT hsaKmtCloseKFD(): the ROCr runtime may keep the thunk
  // open for the process lifetime, and closing here is unnecessary for a
  // read-only topology query. The dlopen handle is deliberately leaked so the
  // library stays resident (matches ROCr thunk_loader behavior).

  return true;
}

}  // namespace amd::smi
