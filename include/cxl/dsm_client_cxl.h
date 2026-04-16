/**
 * dsm_client_cxl.h - CXL 版本的 DSMClient
 * 
 * 這個檔案是整個 CXL 轉換的核心。
 * 
 * 設計原則：
 *   保持與原版 DSMClient 完全相同的 public API (方法簽名)，
 *   這樣 Tree.cpp 可以 #include 這個檔案而不需要修改任何代碼。
 * 
 * 三個核心轉換：
 *   1. RDMA Read/Write → memcpy（因為 CXL 是 load/store）
 *   2. RDMA CAS/FAA   → __sync_val_compare_and_swap / CAS-loop
 *   3. RDMA Poll CQ    → no-op（因為 CXL 操作是同步的）
 * 
 * coroutine (CoroContext) 的處理：
 *   所有方法保留 CoroContext* ctx 參數，但在 CXL 版本中完全忽略。
 *   原版中 ctx != nullptr 時會 yield 等待 RDMA 完成；
 *   CXL 版中操作直接完成，不需要 yield。
 *   Tree.cpp 中 ctx->coro_id 仍會被用來選擇 RdmaBuffer，這是純記憶體操作，保持正確。
 */
#pragma once

#include <atomic>
#include <cstring>
#include <mutex>

#include "cxl/CxlCommon.h"
#include "cxl/dsm_server_cxl.h"
#include "Cache.h"
#include "GlobalAddress.h"
#include "LocalAllocator.h"
#include "RdmaBuffer.h"

class DSMClient {
 public:
  /**
   * GetInstance - 單例模式
   * 
   * 與原版的差異：多了 server 參數，因為 CXL 模式下 client 和 server 在同一個進程。
   * 原版通過 memcached 交換連接資訊，CXL 版直接拿 server 的記憶體指標。
   */
  static DSMClient *GetInstance(const DSMConfig &conf, DSMServer *server) {
    static DSMClient dsm(conf, server);
    return &dsm;
  }

  void ResetThread() { app_id_.store(0); }
  void RegisterThread();
  bool IsRegistered() { return thread_id_ != -1; }

  uint16_t get_my_client_id() { return 0; }  // CXL: 只有一個 client
  uint16_t get_my_thread_id() { return thread_id_; }
  uint16_t get_server_size() { return 1; }   // CXL: 只有一個 server (nodeID=0)
  uint16_t get_client_size() { return 1; }
  uint64_t get_thread_tag() { return thread_tag_; }

  // CXL 下 barrier 不需要 memcached，用簡單的 no-op
  // 因為只有一個 client process，不需要跨進程同步
  void Barrier(const std::string &ss) { (void)ss; }

  char *get_rdma_buffer() { return rdma_buffer_; }
  RdmaBuffer &get_rbuf(int coro_id) { return rbuf_[coro_id]; }

  // ================================================================
  // 核心：所有 RDMA 操作轉換為 memcpy / 原子操作
  // ================================================================

  /**
   * Read / ReadSync - 從遠端讀取數據
   * 
   * RDMA 版本：rdmaRead() → post WR to NIC → NIC 發送請求 → 等待 CQ
   * CXL  版本：memcpy()   → CPU 直接從 NUMA node 1 load 數據
   * 
   * 注意 signal 和 ctx 參數在 CXL 下都被忽略了。
   */
  void Read(char *buffer, GlobalAddress gaddr, size_t size, bool signal = true,
            CoroContext *ctx = nullptr) {
    (void)signal; (void)ctx;
    void *remote_ptr = resolve(gaddr);
    memcpy(buffer, remote_ptr, size);
  }

  void ReadSync(char *buffer, GlobalAddress gaddr, size_t size,
                CoroContext *ctx = nullptr) {
    (void)ctx;
    Read(buffer, gaddr, size);
    // 不需要 pollWithCQ —— memcpy 返回就代表完成了
  }

  /**
   * Write / WriteSync - 寫入數據到遠端
   * 
   * RDMA 版本：rdmaWrite() → NIC 把數據推送到遠端
   * CXL  版本：memcpy()    → CPU 直接 store 到 NUMA node 1
   */
  void Write(const char *buffer, GlobalAddress gaddr, size_t size,
             bool signal = true, CoroContext *ctx = nullptr) {
    (void)signal; (void)ctx;
    void *remote_ptr = resolve(gaddr);
    memcpy(remote_ptr, buffer, size);
  }

  void WriteSync(const char *buffer, GlobalAddress gaddr, size_t size,
                 CoroContext *ctx = nullptr) {
    (void)ctx;
    Write(buffer, gaddr, size);
  }

  /**
   * ReadBatch / WriteBatch - 批量讀寫
   * 
   * RDMA 版本：用 ibv_post_send 的 WR chain 一次提交多個操作
   * CXL  版本：就是循環 memcpy，每個 RdmaOpRegion 做一次
   * 
   * 注意：FillKeysDest() 在原版中把 GlobalAddress 解析成 (dsm_base + offset, rkey)。
   * CXL 版本中我們直接在 memcpy 前解析。
   */
  void ReadBatch(RdmaOpRegion *rs, int k, bool signal = true,
                 CoroContext *ctx = nullptr) {
    (void)signal; (void)ctx;
    for (int i = 0; i < k; ++i) {
      GlobalAddress gaddr;
      gaddr.raw = rs[i].dest;
      void *remote_ptr = rs[i].is_on_chip ? resolve_lock(gaddr) : resolve(gaddr);
      memcpy((void *)rs[i].source, remote_ptr, rs[i].size);
    }
  }

  void ReadBatchSync(RdmaOpRegion *rs, int k, CoroContext *ctx = nullptr) {
    (void)ctx;
    ReadBatch(rs, k);
  }

  void WriteBatch(RdmaOpRegion *rs, int k, bool signal = true,
                  CoroContext *ctx = nullptr) {
    (void)signal; (void)ctx;
    for (int i = 0; i < k; ++i) {
      GlobalAddress gaddr;
      gaddr.raw = rs[i].dest;
      void *remote_ptr = rs[i].is_on_chip ? resolve_lock(gaddr) : resolve(gaddr);
      memcpy(remote_ptr, (void *)rs[i].source, rs[i].size);
    }
  }

  void WriteBatchSync(RdmaOpRegion *rs, int k, CoroContext *ctx = nullptr) {
    (void)ctx;
    WriteBatch(rs, k);
  }

  /**
   * Cas / CasSync - Compare-And-Swap
   * 
   * RDMA 版本：rdmaCompareAndSwap() → NIC 在遠端記憶體上執行原子 CAS
   * CXL  版本：__sync_val_compare_and_swap() → CPU 對 NUMA node 1 執行原子 CAS
   *           x86 架構保證跨 NUMA 的 CAS 是原子的（通過 cache coherence）
   * 
   * 語意：如果 *addr == equal，則 *addr = val，返回舊值到 rdma_buffer
   */
  void Cas(GlobalAddress gaddr, uint64_t equal, uint64_t val,
           uint64_t *rdma_buffer, bool signal = true,
           CoroContext *ctx = nullptr) {
    (void)signal; (void)ctx;
    uint64_t *remote_ptr = (uint64_t *)resolve(gaddr);
    *rdma_buffer = __sync_val_compare_and_swap(remote_ptr, equal, val);
  }

  bool CasSync(GlobalAddress gaddr, uint64_t equal, uint64_t val,
               uint64_t *rdma_buffer, CoroContext *ctx = nullptr) {
    (void)ctx;
    Cas(gaddr, equal, val, rdma_buffer);
    return equal == *rdma_buffer;
  }

  /**
   * CasMask / CasMaskSync - CAS with mask (Extended Atomic)
   * 
   * 這是 Mellanox NIC 的特殊功能：只比較/修改指定 mask 的 bits。
   * x86 CPU 沒有這個指令，需要軟體模擬。
   * 
   * 模擬方式：CAS loop
   *   1. 讀取當前值 old_val
   *   2. 檢查 (old_val & mask) == (equal & mask)
   *   3. 如果匹配，構造新值 new_val = (old_val & ~mask) | (val & mask)
   *   4. 嘗試 CAS(old_val → new_val)，如果失敗則重試
   * 
   * log_sz 表示操作大小：log_sz <= 3 表示 8 字節（64 bit）的操作
   */
  void CasMask(GlobalAddress gaddr, int log_sz, uint64_t equal, uint64_t val,
               uint64_t *rdma_buffer, uint64_t mask = ~(0ull),
               bool signal = true, CoroContext *ctx = nullptr) {
    (void)signal; (void)ctx;
    if (log_sz <= 3) {
      // 8-byte CAS with mask
      uint64_t *ptr = (uint64_t *)resolve(gaddr);
      uint64_t old_val = __atomic_load_n(ptr, __ATOMIC_SEQ_CST);
      *rdma_buffer = old_val;
      // 注意：真正的 CAS 嘗試在 CasMaskSync 中做判斷
    } else {
      // Extended (>8 byte): 在 CXL 實作中暫時只支持 8-byte
      fprintf(stderr, "CasMask: log_sz > 3 not fully supported in CXL mode\n");
      uint64_t *ptr = (uint64_t *)resolve(gaddr);
      *rdma_buffer = __atomic_load_n(ptr, __ATOMIC_SEQ_CST);
    }
  }

  bool CasMaskSync(GlobalAddress gaddr, int log_sz, uint64_t equal,
                   uint64_t val, uint64_t *rdma_buffer, uint64_t mask = ~(0ull),
                   CoroContext *ctx = nullptr) {
    (void)ctx;
    if (log_sz <= 3) {
      uint64_t *ptr = (uint64_t *)resolve(gaddr);
      while (true) {
        uint64_t old_val = __atomic_load_n(ptr, __ATOMIC_SEQ_CST);
        if ((old_val & mask) != (equal & mask)) {
          *rdma_buffer = old_val;
          return false;  // 比較失敗
        }
        // 構造新值：保留 ~mask 的 bits，替換 mask 的 bits
        uint64_t new_val = (old_val & ~mask) | (val & mask);
        if (__sync_bool_compare_and_swap(ptr, old_val, new_val)) {
          *rdma_buffer = old_val;
          return true;  // CAS 成功
        }
        // spurious failure, retry
      }
    } else {
      CasMask(gaddr, log_sz, equal, val, rdma_buffer, mask);
      // 對於 >8 byte 的 extended atomic，暫時做 simplified check
      if (log_sz <= 3) {
        return (equal & mask) == (*rdma_buffer & mask);
      } else {
        uint64_t *eq = (uint64_t *)equal;
        uint64_t *old = (uint64_t *)rdma_buffer;
        uint64_t *m = (uint64_t *)mask;
        for (int i = 0; i < (1 << (log_sz - 3)); i++) {
          if ((eq[i] & m[i]) != (__bswap_64(old[i]) & m[i])) {
            return false;
          }
        }
        return true;
      }
    }
  }

  /**
   * CasMaskWrite - CAS with mask + Write (組合操作)
   * 
   * RDMA 版本：用 WR chain 把 CAS 和 Write 串在一起，一次送給 NIC
   * CXL  版本：先做 CAS，再做 Write。因為是同步的，順序保證了。
   */
  void CasMaskWrite(RdmaOpRegion &cas_ror, uint64_t equal, uint64_t swap,
                    uint64_t mask, RdmaOpRegion &write_ror, bool signal = true,
                    CoroContext *ctx = nullptr) {
    (void)signal; (void)ctx;
    // CAS part
    GlobalAddress cas_gaddr;
    cas_gaddr.raw = cas_ror.dest;
    void *cas_ptr = cas_ror.is_on_chip ? resolve_lock(cas_gaddr) : resolve(cas_gaddr);
    
    if (cas_ror.log_sz <= 3) {
      *(uint64_t *)cas_ror.source =
          __sync_val_compare_and_swap((uint64_t *)cas_ptr, equal, swap);
    } else {
      *(uint64_t *)cas_ror.source = *(uint64_t *)cas_ptr;
    }

    // Write part
    GlobalAddress write_gaddr;
    write_gaddr.raw = write_ror.dest;
    void *write_ptr = write_ror.is_on_chip ? resolve_lock(write_gaddr) : resolve(write_gaddr);
    memcpy(write_ptr, (void *)write_ror.source, write_ror.size);
  }

  bool CasMaskWriteSync(RdmaOpRegion &cas_ror, uint64_t equal, uint64_t swap,
                        uint64_t mask, RdmaOpRegion &write_ror,
                        CoroContext *ctx = nullptr) {
    (void)ctx;
    CasMaskWrite(cas_ror, equal, swap, mask, write_ror);
    if (cas_ror.log_sz <= 3) {
      return (equal & mask) == (*(uint64_t *)cas_ror.source & mask);
    } else {
      uint64_t *eq = (uint64_t *)equal;
      uint64_t *old = (uint64_t *)cas_ror.source;
      uint64_t *m = (uint64_t *)mask;
      for (int i = 0; i < (1 << (cas_ror.log_sz - 3)); ++i) {
        if ((eq[i] & m[i]) != (__bswap_64(old[i]) & m[i])) {
          return false;
        }
      }
      return true;
    }
  }

  /**
   * WriteFaa - Write + Fetch-And-Add (組合操作)
   */
  void WriteFaa(RdmaOpRegion &write_ror, RdmaOpRegion &faa_ror,
                uint64_t add_val, bool signal = true,
                CoroContext *ctx = nullptr) {
    (void)signal; (void)ctx;
    // Write part
    GlobalAddress write_gaddr;
    write_gaddr.raw = write_ror.dest;
    void *write_ptr = write_ror.is_on_chip ? resolve_lock(write_gaddr) : resolve(write_gaddr);
    memcpy(write_ptr, (void *)write_ror.source, write_ror.size);

    // FAA part
    GlobalAddress faa_gaddr;
    faa_gaddr.raw = faa_ror.dest;
    uint64_t *faa_ptr = (uint64_t *)(faa_ror.is_on_chip
                                         ? resolve_lock(faa_gaddr)
                                         : resolve(faa_gaddr));
    *(uint64_t *)faa_ror.source = __sync_fetch_and_add(faa_ptr, add_val);
  }

  void WriteFaaSync(RdmaOpRegion &write_ror, RdmaOpRegion &faa_ror,
                    uint64_t add_val, CoroContext *ctx = nullptr) {
    (void)ctx;
    WriteFaa(write_ror, faa_ror, add_val);
  }

  /**
   * WriteCas - Write + CAS (組合操作)
   */
  void WriteCas(RdmaOpRegion &write_ror, RdmaOpRegion &cas_ror, uint64_t equal,
                uint64_t val, bool signal = true, CoroContext *ctx = nullptr) {
    (void)signal; (void)ctx;
    // Write part
    GlobalAddress write_gaddr;
    write_gaddr.raw = write_ror.dest;
    void *write_ptr = write_ror.is_on_chip ? resolve_lock(write_gaddr) : resolve(write_gaddr);
    memcpy(write_ptr, (void *)write_ror.source, write_ror.size);

    // CAS part
    GlobalAddress cas_gaddr;
    cas_gaddr.raw = cas_ror.dest;
    uint64_t *cas_ptr = (uint64_t *)(cas_ror.is_on_chip
                                         ? resolve_lock(cas_gaddr)
                                         : resolve(cas_gaddr));
    *(uint64_t *)cas_ror.source =
        __sync_val_compare_and_swap(cas_ptr, equal, val);
  }

  void WriteCasSync(RdmaOpRegion &write_ror, RdmaOpRegion &cas_ror,
                    uint64_t equal, uint64_t val, CoroContext *ctx = nullptr) {
    (void)ctx;
    WriteCas(write_ror, cas_ror, equal, val);
  }

  /**
   * CasRead - CAS + Read (組合操作)
   */
  void CasRead(RdmaOpRegion &cas_ror, RdmaOpRegion &read_ror, uint64_t equal,
               uint64_t val, bool signal = true, CoroContext *ctx = nullptr) {
    (void)signal; (void)ctx;
    // CAS part
    GlobalAddress cas_gaddr;
    cas_gaddr.raw = cas_ror.dest;
    uint64_t *cas_ptr = (uint64_t *)(cas_ror.is_on_chip
                                         ? resolve_lock(cas_gaddr)
                                         : resolve(cas_gaddr));
    *(uint64_t *)cas_ror.source =
        __sync_val_compare_and_swap(cas_ptr, equal, val);

    // Read part
    GlobalAddress read_gaddr;
    read_gaddr.raw = read_ror.dest;
    void *read_ptr = read_ror.is_on_chip ? resolve_lock(read_gaddr) : resolve(read_gaddr);
    memcpy((void *)read_ror.source, read_ptr, read_ror.size);
  }

  bool CasReadSync(RdmaOpRegion &cas_ror, RdmaOpRegion &read_ror,
                   uint64_t equal, uint64_t val, CoroContext *ctx = nullptr) {
    (void)ctx;
    CasRead(cas_ror, read_ror, equal, val);
    return equal == *(uint64_t *)cas_ror.source;
  }

  /**
   * FaaRead - Fetch-And-Add + Read (組合操作)
   */
  void FaaRead(RdmaOpRegion &faa_ror, RdmaOpRegion &read_ror, uint64_t add,
               bool signal = true, CoroContext *ctx = nullptr) {
    (void)signal; (void)ctx;
    GlobalAddress faa_gaddr;
    faa_gaddr.raw = faa_ror.dest;
    uint64_t *faa_ptr = (uint64_t *)(faa_ror.is_on_chip
                                         ? resolve_lock(faa_gaddr)
                                         : resolve(faa_gaddr));
    *(uint64_t *)faa_ror.source = __sync_fetch_and_add(faa_ptr, add);

    GlobalAddress read_gaddr;
    read_gaddr.raw = read_ror.dest;
    void *read_ptr = read_ror.is_on_chip ? resolve_lock(read_gaddr) : resolve(read_gaddr);
    memcpy((void *)read_ror.source, read_ptr, read_ror.size);
  }

  void FaaReadSync(RdmaOpRegion &faa_ror, RdmaOpRegion &read_ror, uint64_t add,
                   CoroContext *ctx = nullptr) {
    (void)ctx;
    FaaRead(faa_ror, read_ror, add);
  }

  /**
   * FaaBoundRead - FAA with boundary + Read
   */
  void FaaBoundRead(RdmaOpRegion &faab_ror, RdmaOpRegion &read_ror,
                    uint64_t add, uint64_t boundary, bool signal = true,
                    CoroContext *ctx = nullptr) {
    (void)signal; (void)ctx;
    // FAA with boundary
    GlobalAddress faa_gaddr;
    faa_gaddr.raw = faab_ror.dest;
    uint64_t *faa_ptr = (uint64_t *)(faab_ror.is_on_chip
                                         ? resolve_lock(faa_gaddr)
                                         : resolve(faa_gaddr));
    faa_bound_impl(faa_ptr, add, boundary, (uint64_t *)faab_ror.source);

    // Read part
    GlobalAddress read_gaddr;
    read_gaddr.raw = read_ror.dest;
    void *read_ptr = read_ror.is_on_chip ? resolve_lock(read_gaddr) : resolve(read_gaddr);
    memcpy((void *)read_ror.source, read_ptr, read_ror.size);
  }

  void FaaBoundReadSync(RdmaOpRegion &faab_ror, RdmaOpRegion &read_ror,
                        uint64_t add, uint64_t boundary,
                        CoroContext *ctx = nullptr) {
    (void)ctx;
    FaaBoundRead(faab_ror, read_ror, add, boundary);
  }

  /**
   * FaaBound / FaaBoundSync - Fetch-And-Add with boundary
   * 
   * Mellanox NIC 的 Extended Atomic：
   *   對每個 field (由 boundary mask 定義) 獨立做 FAA，
   *   並且每個 field 在到達 boundary 時 wrap around。
   * 
   * 在 DEFT 中，這用於實現 SX Lock (shared/exclusive lock)：
   *   64-bit lock = [X_CUR:16 | X_TIC:16 | S_CUR:16 | S_TIC:16]
   *   FAA with boundary mask 0x8000800080008000 表示：
   *   每個 16-bit field 獨立 +1，且在 0x8000 (32768) 時 wrap 到 0
   * 
   * CXL 軟體模擬：CAS loop
   */
  void FaaBound(GlobalAddress gaddr, int log_sz, uint64_t add_val,
                uint64_t *rdma_buffer, uint64_t mask, bool signal = true,
                CoroContext *ctx = nullptr) {
    (void)signal; (void)ctx;
    uint64_t *ptr = (uint64_t *)resolve(gaddr);
    faa_bound_impl(ptr, add_val, mask, rdma_buffer);
  }

  void FaaBoundSync(GlobalAddress gaddr, int log_sz, uint64_t add_val,
                    uint64_t *rdma_buffer, uint64_t mask,
                    CoroContext *ctx = nullptr) {
    (void)ctx;
    FaaBound(gaddr, log_sz, add_val, rdma_buffer, mask);
  }

  // ================================================================
  // On-Chip Memory (Device Memory) 操作
  // 在 CXL 中，這些指向 NUMA node 0 的 lock memory
  // ================================================================

  void ReadDm(char *buffer, GlobalAddress gaddr, size_t size,
              bool signal = true, CoroContext *ctx = nullptr) {
    (void)signal; (void)ctx;
    void *lock_ptr = resolve_lock(gaddr);
    memcpy(buffer, lock_ptr, size);
  }

  void ReadDmSync(char *buffer, GlobalAddress gaddr, size_t size,
                  CoroContext *ctx = nullptr) {
    (void)ctx;
    ReadDm(buffer, gaddr, size);
  }

  void WriteDm(const char *buffer, GlobalAddress gaddr, size_t size,
               bool signal = true, CoroContext *ctx = nullptr) {
    (void)signal; (void)ctx;
    void *lock_ptr = resolve_lock(gaddr);
    memcpy(lock_ptr, buffer, size);
  }

  void WriteDmSync(const char *buffer, GlobalAddress gaddr, size_t size,
                   CoroContext *ctx = nullptr) {
    (void)ctx;
    WriteDm(buffer, gaddr, size);
  }

  void CasDm(GlobalAddress gaddr, uint64_t equal, uint64_t val,
             uint64_t *rdma_buffer, bool signal = true,
             CoroContext *ctx = nullptr) {
    (void)signal; (void)ctx;
    uint64_t *lock_ptr = (uint64_t *)resolve_lock(gaddr);
    *rdma_buffer = __sync_val_compare_and_swap(lock_ptr, equal, val);
  }

  bool CasDmSync(GlobalAddress gaddr, uint64_t equal, uint64_t val,
                 uint64_t *rdma_buffer, CoroContext *ctx = nullptr) {
    (void)ctx;
    CasDm(gaddr, equal, val, rdma_buffer);
    return equal == *rdma_buffer;
  }

  void CasDmMask(GlobalAddress gaddr, int log_sz, uint64_t equal, uint64_t val,
                 uint64_t *rdma_buffer, uint64_t mask = ~(0ull),
                 bool signal = true, CoroContext *ctx = nullptr) {
    (void)signal; (void)ctx;
    uint64_t *lock_ptr = (uint64_t *)resolve_lock(gaddr);
    *rdma_buffer = __atomic_load_n(lock_ptr, __ATOMIC_SEQ_CST);
  }

  bool CasDmMaskSync(GlobalAddress gaddr, int log_sz, uint64_t equal,
                     uint64_t val, uint64_t *rdma_buffer,
                     uint64_t mask = ~(0ull), CoroContext *ctx = nullptr) {
    (void)ctx;
    if (log_sz <= 3) {
      uint64_t *ptr = (uint64_t *)resolve_lock(gaddr);
      while (true) {
        uint64_t old_val = __atomic_load_n(ptr, __ATOMIC_SEQ_CST);
        if ((old_val & mask) != (equal & mask)) {
          *rdma_buffer = old_val;
          return false;
        }
        uint64_t new_val = (old_val & ~mask) | (val & mask);
        if (__sync_bool_compare_and_swap(ptr, old_val, new_val)) {
          *rdma_buffer = old_val;
          return true;
        }
      }
    } else {
      return (equal & mask) == (*rdma_buffer & mask);
    }
  }

  /**
   * FaaDmBound - FAA with boundary on lock (on-chip) memory
   * 
   * 這是 DEFT 的 SX Lock 的核心操作！
   * Tree.cpp 中的 acquire_sx_lock() 調用這個方法來獲取鎖。
   */
  void FaaDmBound(GlobalAddress gaddr, int log_sz, uint64_t add_val,
                  uint64_t *rdma_buffer, uint64_t mask, bool signal = true,
                  CoroContext *ctx = nullptr) {
    (void)signal; (void)ctx;
    uint64_t *ptr = (uint64_t *)resolve_lock(gaddr);
    faa_bound_impl(ptr, add_val, mask, rdma_buffer);
  }

  void FaaDmBoundSync(GlobalAddress gaddr, int log_sz, uint64_t add_val,
                      uint64_t *rdma_buffer, uint64_t mask,
                      CoroContext *ctx = nullptr) {
    (void)ctx;
    FaaDmBound(gaddr, log_sz, add_val, rdma_buffer, mask);
  }

  /**
   * PollRdmaCq - 輪詢完成隊列
   * 
   * CXL 版本：所有操作都是同步的，不需要等待完成。直接返回。
   */
  uint64_t PollRdmaCq(int count = 1) {
    (void)count;
    return 0;
  }

  bool PollRdmaCqOnce(uint64_t &wr_id) {
    wr_id = 0;
    return true;
  }

  /**
   * Sum - 跨節點求和
   * 
   * 原版通過 memcached 實現。CXL 單機模式下直接返回自己的值。
   */
  uint64_t Sum(uint64_t value) { return value; }

  /**
   * Alloc - 分配遠端記憶體
   * 
   * 原版流程：
   *   1. LocalAllocator 嘗試從已有 chunk 中分配
   *   2. 如果空間不夠，發 RPC MALLOC 給 server
   *   3. Server 的 Directory 線程調用 GlobalAllocator
   *   4. 返回新 chunk 的 GlobalAddress
   * 
   * CXL 版本：
   *   1. LocalAllocator 嘗試從已有 chunk 中分配（不變）
   *   2. 如果空間不夠，直接調用 server_->alloc_chunk()（代替 RPC）
   */
  GlobalAddress Alloc(size_t size) {
    bool need_chunk = false;
    auto addr = local_allocator_.malloc(size, need_chunk);
    if (need_chunk) {
      // 原版這裡是：RpcCallDir(MALLOC) + RpcWait()
      // CXL 版本直接調用 server 的 allocator
      GlobalAddress chunk_addr = server_->alloc_chunk();
      local_allocator_.set_chunck(chunk_addr);
      // retry
      addr = local_allocator_.malloc(size, need_chunk);
    }
    return addr;
  }

  void Free(GlobalAddress addr) { local_allocator_.free(addr); }

  /**
   * RpcCallDir / RpcWait - RPC 機制
   * 
   * CXL 版本不需要 RPC，但 Tree.cpp 的一些地方（如 TERMINATE）會調用。
   * 我們提供空實現以避免編譯錯誤。
   */
  void RpcCallDir(const RawMessage &m, uint16_t node_id, uint16_t dir_id = 0) {
    (void)m; (void)node_id; (void)dir_id;
    // CXL: no RPC needed
  }
  RawMessage *RpcWait() {
    // 返回一個 dummy message
    static RawMessage dummy;
    return &dummy;
  }

 private:
  DSMConfig conf_;
  DSMServer *server_;
  std::atomic_int app_id_;
  Cache cache_;

  void *dsm_base_;    // data memory base (NUMA node 1)
  void *lock_base_;   // lock memory base (NUMA node 0)

  static thread_local int thread_id_;
  static thread_local char *rdma_buffer_;
  static thread_local LocalAllocator local_allocator_;
  static thread_local RdmaBuffer rbuf_[define::kMaxCoro];
  static thread_local uint64_t thread_tag_;

  DSMClient(const DSMConfig &conf, DSMServer *server)
      : conf_(conf), server_(server), app_id_(0), cache_(conf.cache_size) {
    dsm_base_ = server->get_base_addr();
    lock_base_ = server->get_lock_addr();
    printf("CXL DSMClient: dsm_base=%p, lock_base=%p\n", dsm_base_, lock_base_);
  }

  /**
   * resolve() - 將 GlobalAddress 解析為虛擬地址指標
   * 
   * 這就是 RDMA → CXL 的核心轉換：
   * RDMA: GlobalAddress → (remote rkey, remote VA) → NIC 通過網路存取
   * CXL:  GlobalAddress → dsm_base_ + offset → CPU 直接 load/store
   */
  void *resolve(GlobalAddress gaddr) {
    return (char *)dsm_base_ + gaddr.offset;
  }

  /**
   * resolve_lock() - 解析 lock (on-chip) 地址
   * 
   * Lock 記憶體在 NUMA node 0（本地），所以存取延遲比 data 低。
   */
  void *resolve_lock(GlobalAddress gaddr) {
    return (char *)lock_base_ + gaddr.offset;
  }

  /**
   * faa_bound_impl() - FAA with boundary 的軟體實現
   * 
   * Mellanox NIC 的 ExtendedAtomicsBoundedFetchAndAdd：
   *   boundary mask 定義了每個 field 的邊界。
   *   例如 mask = 0x8000800080008000 表示：
   *   把 64-bit 分成 4 個 16-bit field，每個 field 獨立 FAA，
   *   到達 0x8000 時 wrap 到 0。
   * 
   * 我們用 CAS loop 模擬：
   *   1. 讀取當前值
   *   2. 對每個 field 獨立做加法和 boundary 處理
   *   3. 嘗試 CAS，失敗則重試
   */
  static void faa_bound_impl(uint64_t *ptr, uint64_t add_val,
                              uint64_t boundary_mask, uint64_t *result) {
    while (true) {
      uint64_t old_val = __atomic_load_n(ptr, __ATOMIC_SEQ_CST);

      // 對每個 16-bit field 獨立做 bounded FAA
      // boundary_mask = 0x8000800080008000 表示每個 field 的上界是 0x8000
      uint64_t new_val = 0;
      for (int i = 0; i < 4; ++i) {
        int shift = i * 16;
        uint16_t field = (old_val >> shift) & 0xFFFF;
        uint16_t add_field = (add_val >> shift) & 0xFFFF;
        uint16_t bound = (boundary_mask >> shift) & 0xFFFF;

        uint16_t result_field = field + add_field;
        if (bound != 0 && result_field >= bound) {
          result_field -= bound;  // wrap around
        }
        new_val |= ((uint64_t)result_field << shift);
      }

      if (__sync_bool_compare_and_swap(ptr, old_val, new_val)) {
        *result = old_val;
        return;
      }
      // CAS failed, retry
    }
  }
};

// 需要在 RawMessage 的定義用於 RpcCallDir
// 在 CXL 中這只是一個 dummy struct
struct RawMessage {
  uint8_t type;
  uint16_t node_id;
  uint16_t app_id;
  GlobalAddress addr;
  int level;
} __attribute__((packed));
