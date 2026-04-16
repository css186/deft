/**
 * dsm_server_cxl.h - CXL 版本的 DSMServer
 * 
 * 原版 DSMServer 做了什麼？
 *   1. hugePageAlloc() 分配一大塊記憶體 (dsm_size GB)
 *   2. 用 RDMA MR 註冊這塊記憶體，讓遠端 client 可以 RDMA 存取
 *   3. 啟動 Directory 線程，處理 RPC 請求（主要是 MALLOC）
 *   4. 通過 memcached 與 client 交換 QP/rkey 資訊
 * 
 * CXL 版本只需要做 #1：分配記憶體。
 * 因為 CXL 是 load/store，不需要 QP/MR；MALLOC 改成直接函式調用。
 * 
 * 記憶體分配策略：
 *   - Data memory → NUMA node 1 (模擬 CXL attached memory, 跨 socket 延遲)
 *   - Lock memory → NUMA node 0 (模擬 on-chip device memory, 本地低延遲)
 *     這是 DEFT 的特色：鎖放在 NIC 的 on-chip memory 上以降低鎖競爭延遲
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <numa.h>
#include <numaif.h>
#include <sys/mman.h>

#include "cxl/CxlCommon.h"
#include "GlobalAddress.h"
#include "GlobalAllocator.h"
#include "Config.h"

class DSMServer {
 public:
  static DSMServer* GetInstance(const DSMConfig& conf) {
    static DSMServer server(conf);
    return &server;
  }

  // 提供給 DSMClient 使用的接口
  void* get_base_addr() const { return (void*)base_addr_; }
  void* get_lock_addr() const { return lock_pool_; }
  uint64_t get_dsm_size() const { return dsm_size_; }

  /**
   * alloc_chunk() - 分配一個 chunk (16MB)
   * 
   * 原版中這是通過 RPC 完成的：
   *   Client 發 RPC MALLOC → Server Directory 線程收到 → 調用 GlobalAllocator
   * 
   * CXL 版本：直接調用，因為 client 和 server 在同一個進程裡。
   * 注意：需要加鎖因為多個 client 線程可能同時 malloc。
   */
  GlobalAddress alloc_chunk() {
    std::lock_guard<std::mutex> guard(alloc_mutex_);
    return chunk_alloc_->alloc_chunck();
  }

  // CXL 版不需要 Run() 來啟動 Directory 線程
  // void Run();

 private:
  uint64_t base_addr_;     // 資料記憶體基底地址 (在 NUMA node 1 上)
  void* lock_pool_;        // 鎖記憶體 (在 NUMA node 0 上，模擬 on-chip)
  uint64_t dsm_size_;      // 資料記憶體大小 (bytes)
  GlobalAllocator* chunk_alloc_;  // chunk-level 記憶體分配器
  std::mutex alloc_mutex_;        // 保護 allocator 的鎖

  DSMServer(const DSMConfig& conf) {
    dsm_size_ = (uint64_t)conf.dsm_size * define::GB;

    // ================================================================
    // 第一步：在 NUMA node 1 上分配 data memory
    // ================================================================
    // 這就是「CXL attached memory」—— 存取延遲比本地 DRAM 高
    // 
    // 我們用 mmap + mbind 而不是 numa_alloc_onnode，因為需要 hugepage 支持。
    // 原版用的也是 hugePageAlloc()，只是分配在本地。
    //
    // 先用 mmap 分配，然後用 mbind 強制綁定到 NUMA node 1
    base_addr_ = (uint64_t)mmap(NULL, dsm_size_, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, 
                                 -1, 0);
    if ((void*)base_addr_ == MAP_FAILED) {
      fprintf(stderr, "CXL DSMServer: mmap failed for data memory (%lu GB)!\n",
              conf.dsm_size);
      fprintf(stderr, "Make sure hugepages are available on NUMA node 1.\n");
      // Fallback: 不用 hugepage
      base_addr_ = (uint64_t)mmap(NULL, dsm_size_, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if ((void*)base_addr_ == MAP_FAILED) {
        perror("mmap fallback also failed");
        exit(1);
      }
      fprintf(stderr, "CXL DSMServer: using regular pages (fallback)\n");
    }

    // 把這塊記憶體綁定到 NUMA node 1
    // numa_bitmask: 表示只用 node 1
    if (numa_available() >= 0) {
      unsigned long node_mask = 1UL << 1;  // NUMA node 1
      int ret = mbind((void*)base_addr_, dsm_size_, MPOL_BIND,
                       &node_mask, sizeof(node_mask) * 8, 
                       MPOL_MF_MOVE | MPOL_MF_STRICT);
      if (ret != 0) {
        perror("mbind to NUMA node 1 failed");
        fprintf(stderr, "Warning: data memory may not be on NUMA node 1.\n");
        fprintf(stderr, "This is OK for functional testing, but cross-NUMA latency won't be accurate.\n");
      } else {
        printf("CXL DSMServer: data memory bound to NUMA node 1 (%lu GB)\n",
               conf.dsm_size);
      }
    } else {
      fprintf(stderr, "Warning: NUMA not available, running without NUMA binding.\n");
    }

    // ================================================================
    // 第二步：Warmup（和原版一樣）
    // ================================================================
    // 觸碰每一頁讓 OS 真正分配物理頁面
    for (uint64_t i = 0; i < dsm_size_; i += 2 * define::MB) {
      *(char*)(base_addr_ + i) = 0;
    }
    // 清空第一個 chunk（存放 root pointer 等 metadata）
    memset((char*)base_addr_, 0, define::kChunkSize);

    // ================================================================
    // 第三步：在 NUMA node 0 上分配 lock memory（模擬 on-chip device memory）
    // ================================================================
    // 為什麼放 NUMA 0？
    //   DEFT 的 on-chip memory 是 NIC 上的 SRAM，特點是延遲 < DRAM。
    //   NUMA 0 是計算線程的本地 socket，存取延遲最低，最接近 on-chip 的效果。
    // 
    // 大小：kLockChipMemSize = 128KB（和原版一樣）
    lock_pool_ = numa_available() >= 0
                     ? numa_alloc_onnode(define::kLockChipMemSize, 0)
                     : malloc(define::kLockChipMemSize);
    memset(lock_pool_, 0, define::kLockChipMemSize);
    printf("CXL DSMServer: lock memory on NUMA node 0 (%lu KB)\n",
           define::kLockChipMemSize / 1024);

    // ================================================================
    // 第四步：初始化 chunk allocator
    // ================================================================
    // 原版中每個 Directory 線程有自己的 GlobalAllocator
    // CXL 版本只有一個（因為只有一個 server process），用 mutex 保護
    GlobalAddress dsm_start;
    dsm_start.nodeID = 0;
    dsm_start.offset = 0;
    chunk_alloc_ = new GlobalAllocator(dsm_start, dsm_size_);

    printf("CXL DSMServer: initialized with %lu GB data memory\n",
           conf.dsm_size);
  }
};
