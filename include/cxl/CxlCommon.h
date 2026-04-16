/**
 * CxlCommon.h - CXL version of Common.h
 * 
 * 這個檔案的目的：提供所有 Tree.cpp 需要的基礎定義，但不依賴 RDMA。
 * 
 * 與原版 Common.h 的差異：
 *   1. 移除 #include "Rdma.h" (不需要 ibverbs)
 *   2. 內嵌一個簡化版的 RdmaOpRegion (Tree.cpp 用這個結構來描述批量操作)
 *   3. 其他所有東西保持不變 (CoroContext, 常量, Key/Value 定義等)
 * 
 * 設計原則：讓 Tree.cpp 可以完全不修改就使用這個 header。
 */
#ifndef __CXL_COMMON_H__
#define __CXL_COMMON_H__

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <atomic>
#include <bitset>
#include <limits>

#include "Debug.h"
#include "HugePageAlloc.h"

// ====================================================================
// 原本這裡是 #include "Rdma.h"
// 但 CXL 不需要 RDMA，所以我們只保留 Tree.cpp 實際用到的結構：
// ====================================================================

#define forceinline inline __attribute__((always_inline))

/**
 * RdmaOpRegion - 描述一個讀/寫/原子操作的區域
 * 
 * 在 RDMA 版本中，這用來告訴 NIC：
 *   - source: 本地 buffer 地址 (CPU 這邊)
 *   - dest:   遠端地址 (server 那邊)
 *   - size:   操作大小
 *   - lkey:   本地記憶體的 key (RDMA 用)
 *   - remoteRKey: 遠端記憶體的 key (RDMA 用)
 * 
 * 在 CXL 版本中，lkey 和 remoteRKey 不再需要，但我們保留結構不改，
 * 因為 Tree.cpp 裡面大量使用了 RdmaOpRegion 來組織操作。
 * 改名成本太高，也不影響正確性。
 */
constexpr int kOroMax = 3;
struct RdmaOpRegion {
  uint64_t source;    // 本地 buffer 地址
  uint64_t dest;      // 在 CXL 中，這存的是 GlobalAddress.raw
  union {
    uint64_t size;    // 讀/寫的大小
    uint64_t log_sz;  // extended atomic 操作的 log2(size)
  };

  uint32_t lkey;      // CXL 下不使用，但保留結構
  union {
    uint32_t remoteRKey;  // CXL 下不使用
    bool is_on_chip;      // ← 這個仍然重要！區分 data vs lock 記憶體
  };
};

// ====================================================================
// 以下完全和原版 Common.h 相同，不需要改
// ====================================================================

#include "WRLock.h"

#define KEY_SIZE 8

#define LATENCY_WINDOWS 1000000

#define STRUCT_OFFSET(type, field) \
  (char *)&((type *)(0))->field - (char *)((type *)(0))

#define MAX_MACHINE 8

#define ADD_ROUND(x, n) ((x) = ((x) + 1) % (n))

#define MESSAGE_SIZE 96

#define POST_RECV_PER_RC_QP 128

#define RAW_RECV_CQ_COUNT 128

// { app thread
#define MAX_APP_THREAD 32

#define APP_MESSAGE_NR 96
// }

// { dir thread
#define NR_DIRECTORY 1

#define DIR_MESSAGE_NR 128
// }

void bindCore(uint16_t core);
char *getIP();
char *getMac();

inline int bits_in(std::uint64_t u) {
  auto bs = std::bitset<64>(u);
  return bs.count();
}

// Coroutine 定義 - 保留不變
// Tree.cpp 的 coroutine worker/master 依賴這些型別
#include <boost/coroutine/all.hpp>

using CoroYield = boost::coroutines::symmetric_coroutine<void>::yield_type;
using CoroCall = boost::coroutines::symmetric_coroutine<void>::call_type;

struct CoroContext {
  CoroYield *yield;
  CoroCall *master;
  int coro_id;
};

namespace define {

constexpr uint64_t MB = 1024ull * 1024;
constexpr uint64_t GB = 1024ull * MB;
constexpr uint16_t kCacheLineSize = 64;

// for remote allocate
constexpr uint64_t kChunkSize = 16 * MB;

// for store root pointer
constexpr uint64_t kRootPointerStoreOffest = kChunkSize / 2;
static_assert(kRootPointerStoreOffest % sizeof(uint64_t) == 0, "XX");

// lock memory
// 在 CXL 版本中，這塊記憶體存放在 NUMA node 0 (本地 socket)
// 模擬 RDMA NIC 上的 on-chip device memory (低延遲)
constexpr uint64_t kLockStartAddr = 0;
constexpr uint64_t kLockChipMemSize = 128 * 1024;

constexpr uint64_t kLockSize = 16;
constexpr uint64_t kNumOfLock = kLockChipMemSize / kLockSize;

constexpr uint64_t kMaxLevelOfTree = 7;

constexpr uint16_t kMaxCoro = 8;
constexpr int64_t kPerThreadRdmaBuf = 12 * MB;
constexpr int64_t kPerCoroRdmaBuf = kPerThreadRdmaBuf / kMaxCoro;

constexpr uint8_t kMaxHandOverTime = 0;

constexpr int kIndexCacheSize = 1024;  // MB
}  // namespace define

static inline unsigned long long asm_rdtsc(void) {
  unsigned hi, lo;
  __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
  return ((unsigned long long)lo) | (((unsigned long long)hi) << 32);
}

// For Tree
using Key = uint64_t;
using Value = uint64_t;
constexpr Key kKeyMin = std::numeric_limits<Key>::min();
constexpr Key kKeyMax = std::numeric_limits<Key>::max();
constexpr Value kValueNull = 0;

#if KEY_SIZE == 8
using InternalKey = Key;

#else
constexpr size_t arr_cnt = KEY_SIZE / sizeof(uint64_t);

struct KeyArr {
  uint64_t arr[arr_cnt];
  KeyArr() { arr[0] = 0; };
  KeyArr(const uint64_t k) {
    for (size_t i = 0; i < arr_cnt; ++i) arr[i] = k;
  };

  KeyArr &operator=(const KeyArr &other) {
    for (size_t i = 0; i < arr_cnt; ++i) arr[i] = other.arr[i];
    return *this;
  }

  operator uint64_t() const { return arr[0]; }
  bool operator==(const KeyArr &other) const { return arr[0] == other.arr[0]; }
  bool operator!=(const KeyArr &other) const { return arr[0] != other.arr[0]; }

  bool operator==(const uint64_t &other) const { return arr[0] == other; }
  bool operator!=(const uint64_t &other) const { return arr[0] != other; }

  auto operator<=>(const uint64_t &other) const { return arr[0] <=> other; }

  auto operator<=>(const KeyArr &other) const {
    return arr[0] <=> other.arr[0];
  }
} __attribute__((packed));

using InternalKey = KeyArr;

#endif

// fixed for variable length key
constexpr size_t kHeaderRawSize = 30 + 2 * sizeof(InternalKey) + 16;
constexpr size_t kHeaderSize = (kHeaderRawSize + 63) / 64 * 64;
constexpr uint32_t kPageSize =
    (kHeaderSize + 60 * (sizeof(InternalKey) + sizeof(uint64_t)) + 63) / 64 *
    64;
constexpr uint32_t kInternalPageSize = kPageSize;
constexpr uint32_t kLeafPageSize = kPageSize;

__inline__ unsigned long long rdtsc(void) {
  unsigned hi, lo;
  __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
  return ((unsigned long long)lo) | (((unsigned long long)hi) << 32);
}

inline void mfence() { asm volatile("mfence" ::: "memory"); }

inline void compiler_barrier() { asm volatile("" ::: "memory"); }

#endif /* __CXL_COMMON_H__ */
