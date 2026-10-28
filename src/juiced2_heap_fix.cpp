// Juiced 2: Hot Import Nights - recompiled
//
// Heap "delayed free" workaround.
//
// The game sometimes frees a small heap block while another thread is still using it
// (a use-after-free that the original Xbox 360 timing happened to hide). On a PC, where the
// threads really run in parallel, that crashes the game within minutes.
//
// This hook keeps every freed small block (still marked "in use") until a few thousand later
// frees have happened, and only then really frees it. A second free of a block that is still
// waiting (or of a block that is not in use) is ignored. Large blocks and anything unusual
// pass straight through to the game's own code.

#include <rex/hook.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>

// The game's own HeapFree (the original recompiled code).
REX_EXTERN(__imp__sub_823DAF48);

namespace {

struct DelayedFree {
  uint32_t heap, flags, ptr, bytes;
};

constexpr size_t kMaxEntries = 16384;
constexpr uint64_t kMaxBytes = 96ull * 1024ull * 1024ull;
constexpr uint32_t kMaxBlockBytes = 65536;

std::mutex g_mu;
std::deque<DelayedFree> g_queue;
std::unordered_map<uint32_t, DelayedFree> g_waiting;
uint64_t g_queued_bytes = 0;

std::atomic<uint64_t> g_delayed{0};
std::atomic<uint64_t> g_ignored{0};
thread_local bool t_inside = false;

bool LoadGuestU32(uint8_t* base, uint32_t addr, uint32_t* out) {
  const uint32_t off = (addr >= 0xE0000000u) ? 0x1000u : 0u;
  __try {
    *out = __builtin_bswap32(*(volatile uint32_t*)(base + addr + off));
    return true;
  } __except (1) {
    return false;
  }
}

void RealFree(PPCContext& ctx, uint8_t* base, const DelayedFree& e) {
  t_inside = true;
  ctx.r3.u64 = e.heap;
  ctx.r4.u64 = e.flags;
  ctx.r5.u64 = e.ptr;
  __imp__sub_823DAF48(ctx, base);
  t_inside = false;
}

}  // namespace

REX_HOOK_RAW(sub_823DAF48) {
  if (t_inside) {
    __imp__sub_823DAF48(ctx, base);
    return;
  }

  const uint32_t heap = ctx.r3.u32;
  const uint32_t flags = ctx.r4.u32;
  const uint32_t ptr = ctx.r5.u32;
  const uint64_t saved_lr = ctx.lr;

  uint32_t w0 = 0, w4 = 0;
  if (ptr == 0 || ptr < 16 || !LoadGuestU32(base, ptr - 16u, &w0) ||
      !LoadGuestU32(base, ptr - 12u, &w4)) {
    __imp__sub_823DAF48(ctx, base);
    return;
  }

  const unsigned flag_byte = (w4 >> 16) & 0xFFu;
  if (!(flag_byte & 1u)) {
    // Block is not in use: freeing it again would corrupt the heap. Ignore.
    const uint64_t n = g_ignored.fetch_add(1) + 1;
    if (n <= 20) {
      REXKRNL_WARN("juiced2 heap fix: free of a block that is not in use ignored (block {:08X}, caller {:08X})",
                   ptr, static_cast<uint32_t>(saved_lr));
    }
    ctx.r3.u64 = 1;
    return;
  }
  if (flag_byte & 8u) {  // large block: leave to the game
    __imp__sub_823DAF48(ctx, base);
    return;
  }
  const uint32_t bytes = (w0 >> 16) * 16u;
  if (bytes == 0 || bytes > kMaxBlockBytes) {
    __imp__sub_823DAF48(ctx, base);
    return;
  }

  DelayedFree release[64];
  int release_count = 0;
  bool duplicate = false;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_waiting.find(ptr) != g_waiting.end()) {
      duplicate = true;
    } else {
      DelayedFree e{heap, flags, ptr, bytes};
      g_queue.push_back(e);
      g_waiting[ptr] = e;
      g_queued_bytes += bytes;
      while ((g_queue.size() > kMaxEntries || g_queued_bytes > kMaxBytes) && release_count < 64) {
        DelayedFree old = g_queue.front();
        g_queue.pop_front();
        g_waiting.erase(old.ptr);
        g_queued_bytes -= old.bytes;
        release[release_count++] = old;
      }
    }
  }

  if (duplicate) {
    const uint64_t n = g_ignored.fetch_add(1) + 1;
    if (n <= 20) {
      REXKRNL_WARN("juiced2 heap fix: second free of a waiting block ignored (block {:08X}, caller {:08X})",
                   ptr, static_cast<uint32_t>(saved_lr));
    }
    ctx.r3.u64 = 1;
    return;
  }

  const uint64_t d = g_delayed.fetch_add(1) + 1;
  for (int i = 0; i < release_count; ++i) {
    RealFree(ctx, base, release[i]);
  }
  ctx.lr = saved_lr;
  ctx.r3.u64 = 1;
  ctx.r4.u64 = flags;
  ctx.r5.u64 = ptr;
  if (d == 1 || d % 200000u == 0) {
    REXKRNL_INFO("juiced2 heap fix: {} frees delayed so far, {} ignored", d, g_ignored.load());
  }
}
