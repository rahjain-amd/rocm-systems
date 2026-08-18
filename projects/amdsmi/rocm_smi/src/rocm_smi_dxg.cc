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

#include <chrono>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

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
using fn_mem = HSAKMT_STATUS (*)(HSAuint32, HSAuint32, HsaMemoryProperties*);

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
  auto getmem = reinterpret_cast<fn_mem>(dlsym(h, "hsaKmtGetNodeMemoryProperties"));
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

    // Static clocks (P1): the DXG topology exposes only the maxima. Live values
    // (if any) come later from the D3DKMT path; here we capture max gfx clock.
    info.max_gfx_clk_mhz = np.MaxEngineClockMhzFCompute;

    // Windows adapter LUID: lets DxgQueryLiveStats() match this GPU to a D3DKMT
    // adapter. A zero LUID means "unknown"; the live query then falls back to
    // the first adapter (correct for the single-GPU WSL case).
    info.luid_low = np.LuidLowPart;
    info.luid_high = static_cast<int32_t>(np.LuidHighPart);
    info.luid_valid = (np.LuidLowPart != 0 || np.LuidHighPart != 0);

    // VRAM total + max memory clock: walk the node's memory banks. The DXG
    // topology reports VRAM as FRAME_BUFFER heaps (public + private); sum their
    // sizes for the total and take the frame-buffer bank's MemoryClockMax as the
    // max MCLK. NumMemoryBanks-sized array per the hsaKmt contract.
    if (getmem && np.NumMemoryBanks > 0) {
      const HSAuint32 kMaxBanks = 32;
      HSAuint32 nb = np.NumMemoryBanks;
      if (nb > kMaxBanks) {
        nb = kMaxBanks;
      }
      HsaMemoryProperties banks[kMaxBanks];
      memset(banks, 0, sizeof(banks));
      HSAKMT_STATUS ms = getmem(i, nb, banks);
      if (ms == HSAKMT_STATUS_SUCCESS) {
        for (HSAuint32 b = 0; b < nb; ++b) {
          if (banks[b].HeapType == HSA_HEAPTYPE_FRAME_BUFFER_PUBLIC ||
              banks[b].HeapType == HSA_HEAPTYPE_FRAME_BUFFER_PRIVATE) {
            info.vram_total_bytes += banks[b].SizeInBytes;
            if (banks[b].MemoryClockMax > info.max_mem_clk_mhz) {
              info.max_mem_clk_mhz = banks[b].MemoryClockMax;
            }
          }
        }
      } else {
        ss << __PRETTY_FUNCTION__ << " | node " << i << " GetNodeMemoryProperties -> " << ms;
        LOG_INFO(ss);
      }
    }
    // Fall back to the node-level LocalMemSize if the bank walk yielded nothing.
    if (info.vram_total_bytes == 0) {
      info.vram_total_bytes = np.LocalMemSize;
    }

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
       << " location_id=" << info.location_id << " name='" << info.name << "'"
       << " max_gfx_mhz=" << info.max_gfx_clk_mhz << " max_mem_mhz=" << info.max_mem_clk_mhz
       << " vram_total=" << info.vram_total_bytes << " luid=" << info.luid_high << ":"
       << info.luid_low;
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

namespace {

// ---------------------------------------------------------------------------
// Minimal, self-contained D3DKMT declarations for the live-statistics query.
//
// We deliberately do NOT pull in the full Windows/WDK headers (they require a
// large platform shim). Instead we hand-roll only the two entry points and the
// handful of structures we touch. The layout of D3DKMT_QUERYSTATISTICS is
// pinned by the Microsoft header invariant C_ASSERT(sizeof==0x328) on AMD64;
// the static_asserts below reproduce that contract so any layout drift fails
// the build rather than silently reading garbage from the kernel.
// ---------------------------------------------------------------------------
struct DXG_LUID {
  uint32_t LowPart;
  int32_t HighPart;
};

struct DXG_ADAPTERINFO {
  uint32_t hAdapter;
  DXG_LUID AdapterLuid;
  uint32_t NumOfSources;
  int32_t bPrecisePresentRegionsPreferred;
};

struct DXG_ENUMADAPTERS2 {
  uint32_t NumAdapters;
  uint32_t _pad;              // pAdapters is 8-byte aligned on LP64
  DXG_ADAPTERINFO* pAdapters;
};

enum {
  kQsAdapter = 0,
  kQsSegment = 3,
  kQsNode = 5,
};

struct QsAdapterInfo {
  uint32_t NbSegments;
  uint32_t NodeCount;
  uint32_t VidPnSourceCount;
};

struct QsSegmentInfo {
  uint64_t CommitLimit;    // segment capacity (bytes)
  uint64_t BytesCommitted;
  uint64_t BytesResident;  // currently resident (bytes) -- "used"
  uint64_t Mem_TotalBytesEvicted;
  uint32_t Mem_AllocsCommitted;
  uint32_t Mem_AllocsResident;
  uint32_t Aperture;       // boolean: 1 => aperture (system) segment, skip for VRAM
};

struct QsNodeInfo {
  uint64_t RunningTime;    // GlobalInformation.RunningTime.QuadPart (microseconds)
};

struct DXG_QUERYSTATISTICS {
  uint32_t Type;           // off 0
  DXG_LUID AdapterLuid;    // off 4
  uint64_t hProcess;       // off 16 (8-byte aligned; 4 bytes pad after LUID)
  union {                  // off 24; real union is 776 bytes
    QsAdapterInfo Adapter;
    QsSegmentInfo Segment;
    QsNodeInfo Node;
    uint8_t _pad[776];
  } QueryResult;
  union {                  // off 800
    uint32_t SegmentId;
    uint32_t NodeId;
  } Query;
};

static_assert(sizeof(DXG_QUERYSTATISTICS) == 0x328,
              "D3DKMT_QUERYSTATISTICS layout drift (expected 0x328 on AMD64)");
static_assert(offsetof(DXG_QUERYSTATISTICS, QueryResult) == 24, "QueryResult offset");
static_assert(offsetof(DXG_QUERYSTATISTICS, Query) == 800, "Query offset");

using fn_enum2 = int32_t (*)(DXG_ENUMADAPTERS2*);
using fn_qs = int32_t (*)(DXG_QUERYSTATISTICS*);

int64_t steady_usec() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

bool DxgQueryLiveStats(uint32_t luid_low, int32_t luid_high, bool luid_valid, uint32_t sample_ms,
                       DxgLiveStats* out) {
  std::ostringstream ss;
  if (out == nullptr) {
    return false;
  }
  *out = DxgLiveStats{};

  // libdxcore.so exports the D3DKMT ABI on WSL. It lives in /usr/lib/wsl/lib,
  // which is on the default WSL loader path; dlopen by soname so native Linux
  // (where it is absent) simply fails and we return cleanly.
  void* h = ::dlopen("libdxcore.so", RTLD_NOW | RTLD_GLOBAL);
  if (h == nullptr) {
    ss << __PRETTY_FUNCTION__ << " | dlopen(libdxcore.so) failed: " << dlerror();
    LOG_INFO(ss);
    return false;
  }

  auto Enum2 = reinterpret_cast<fn_enum2>(dlsym(h, "D3DKMTEnumAdapters2"));
  auto QueryStats = reinterpret_cast<fn_qs>(dlsym(h, "D3DKMTQueryStatistics"));
  if (!Enum2 || !QueryStats) {
    ss << __PRETTY_FUNCTION__ << " | D3DKMT symbols missing in libdxcore.so";
    LOG_ERROR(ss);
    return false;
  }

  DXG_ENUMADAPTERS2 ea;
  memset(&ea, 0, sizeof(ea));
  if (Enum2(&ea) != 0 || ea.NumAdapters == 0) {
    ss << __PRETTY_FUNCTION__ << " | D3DKMTEnumAdapters2 (count) failed / no adapters";
    LOG_ERROR(ss);
    return false;
  }
  const uint32_t kMaxAdapters = 16;
  if (ea.NumAdapters > kMaxAdapters) {
    ea.NumAdapters = kMaxAdapters;
  }
  DXG_ADAPTERINFO adapters[kMaxAdapters];
  memset(adapters, 0, sizeof(adapters));
  ea.pAdapters = adapters;
  if (Enum2(&ea) != 0 || ea.NumAdapters == 0) {
    ss << __PRETTY_FUNCTION__ << " | D3DKMTEnumAdapters2 (fill) failed";
    LOG_ERROR(ss);
    return false;
  }

  // Pick the adapter matching the hsaKmt LUID; else fall back to the first.
  DXG_LUID luid = adapters[0].AdapterLuid;
  if (luid_valid) {
    for (uint32_t a = 0; a < ea.NumAdapters; ++a) {
      if (adapters[a].AdapterLuid.LowPart == luid_low &&
          adapters[a].AdapterLuid.HighPart == luid_high) {
        luid = adapters[a].AdapterLuid;
        break;
      }
    }
  }

  // Adapter-level query: segment + node counts.
  uint32_t nb_seg = 0;
  uint32_t n_node = 0;
  {
    DXG_QUERYSTATISTICS q;
    memset(&q, 0, sizeof(q));
    q.Type = kQsAdapter;
    q.AdapterLuid = luid;
    if (QueryStats(&q) == 0) {
      nb_seg = q.QueryResult.Adapter.NbSegments;
      n_node = q.QueryResult.Adapter.NodeCount;
    }
  }

  // VRAM used = sum of BytesResident over local (non-aperture) segments.
  if (nb_seg > 0) {
    uint64_t used = 0;
    bool any = false;
    for (uint32_t s = 0; s < nb_seg; ++s) {
      DXG_QUERYSTATISTICS q;
      memset(&q, 0, sizeof(q));
      q.Type = kQsSegment;
      q.AdapterLuid = luid;
      q.Query.SegmentId = s;
      if (QueryStats(&q) != 0) {
        continue;
      }
      any = true;
      if (q.QueryResult.Segment.Aperture == 0) {
        used += q.QueryResult.Segment.BytesResident;
      }
    }
    if (any) {
      out->vram_used_bytes = used;
      out->vram_used_valid = true;
    }
  }

  // Engine utilization: sample each node's running time twice over sample_ms and
  // take the busiest node's delta as the GPU's gfx activity (matches how Task
  // Manager reports a single GPU-utilization figure across engines).
  if (n_node > 0) {
    const uint32_t kMaxNodes = 64;
    if (n_node > kMaxNodes) {
      n_node = kMaxNodes;
    }
    uint64_t rt0[kMaxNodes];
    memset(rt0, 0, sizeof(rt0));
    bool sampled0[kMaxNodes];
    memset(sampled0, 0, sizeof(sampled0));

    int64_t t0 = steady_usec();
    for (uint32_t n = 0; n < n_node; ++n) {
      DXG_QUERYSTATISTICS q;
      memset(&q, 0, sizeof(q));
      q.Type = kQsNode;
      q.AdapterLuid = luid;
      q.Query.NodeId = n;
      if (QueryStats(&q) == 0) {
        rt0[n] = q.QueryResult.Node.RunningTime;
        sampled0[n] = true;
      }
    }
    if (sample_ms == 0) {
      sample_ms = 250;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(sample_ms));
    int64_t t1 = steady_usec();
    double wall_us = static_cast<double>(t1 - t0);

    double max_busy = 0.0;
    bool any = false;
    if (wall_us > 0.0) {
      for (uint32_t n = 0; n < n_node; ++n) {
        if (!sampled0[n]) {
          continue;
        }
        DXG_QUERYSTATISTICS q;
        memset(&q, 0, sizeof(q));
        q.Type = kQsNode;
        q.AdapterLuid = luid;
        q.Query.NodeId = n;
        if (QueryStats(&q) != 0) {
          continue;
        }
        any = true;
        uint64_t rt1 = q.QueryResult.Node.RunningTime;
        if (rt1 > rt0[n]) {
          double busy = 100.0 * static_cast<double>(rt1 - rt0[n]) / wall_us;
          if (busy > max_busy) {
            max_busy = busy;
          }
        }
      }
    }
    if (any) {
      if (max_busy > 100.0) {
        max_busy = 100.0;
      }
      out->gfx_activity_pct = static_cast<uint32_t>(max_busy + 0.5);
      out->gfx_activity_valid = true;
    }
  }

  ss << __PRETTY_FUNCTION__ << " | vram_used=" << out->vram_used_bytes << " ("
     << (out->vram_used_valid ? "valid" : "N/A") << ") gfx_activity=" << out->gfx_activity_pct
     << "% (" << (out->gfx_activity_valid ? "valid" : "N/A") << ")";
  LOG_INFO(ss);

  // The dlopen handle is intentionally leaked (kept resident) to match the
  // thunk-loader lifetime policy used by DxgEnumerateGpuNodes above.
  return out->vram_used_valid || out->gfx_activity_valid;
}

}  // namespace amd::smi
