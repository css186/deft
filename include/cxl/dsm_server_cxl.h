/**
 * dsm_server_cxl.h - CXL 版本的 DSMServer
 * 
 * 記憶體分配策略：
 *   - Data memory → remote NUMA node (模擬 CXL attached memory, 跨 socket 延遲)
 *   - Lock memory → local NUMA node  (模擬 on-chip device memory, 本地低延遲)
 * 
 * NUMA node 編號通過參數傳入，支持不同拓撲的機器：
 *   - r6525 (AMD EPYC, NPS4): 16 NUMA nodes, Socket 0 = 0-7, Socket 1 = 8-15
 *   - c6525-100g (NPS1):       2 NUMA nodes, Socket 0 = 0, Socket 1 = 1
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <numa.h>
#include <numaif.h>
#include <sys/mman.h>

#include "cxl/CxlCommon.h"
#include "GlobalAddress.h"
#include "GlobalAllocator.h"
#include "Config.h"

class DSMServer {
 public:
  /**
   * GetInstance - 單例模式
   * 
   * @param conf       DSM 配置
   * @param data_node  Data memory 的 NUMA node (跨 socket，模擬 CXL)
   * @param lock_node  Lock memory 的 NUMA node (本地 socket，模擬 on-chip)
   */
  static DSMServer* GetInstance(const DSMConfig& conf,
                                 int data_node = 8,
                                 int lock_node = 0) {
    static DSMServer server(conf, data_node, lock_node);
    return &server;
  }

  void* get_base_addr() const { return (void*)base_addr_; }
  void* get_lock_addr() const { return lock_pool_; }
  uint64_t get_dsm_size() const { return dsm_size_; }

  GlobalAddress alloc_chunk() {
    std::lock_guard<std::mutex> guard(alloc_mutex_);
    return chunk_alloc_->alloc_chunck();
  }

 private:
  uint64_t base_addr_;
  void* lock_pool_;
  uint64_t dsm_size_;
  GlobalAllocator* chunk_alloc_;
  std::mutex alloc_mutex_;

  DSMServer(const DSMConfig& conf, int data_node, int lock_node) {
    dsm_size_ = (uint64_t)conf.dsm_size * define::GB;

    printf("CXL DSMServer: data NUMA node = %d, lock NUMA node = %d\n",
           data_node, lock_node);

    // ================================================================
    // 第一步：分配 data memory + 綁定到指定 NUMA node
    // ================================================================
    base_addr_ = (uint64_t)mmap(NULL, dsm_size_, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, 
                                 -1, 0);
    if ((void*)base_addr_ == MAP_FAILED) {
      fprintf(stderr, "CXL DSMServer: hugepage mmap failed (%u GB), trying regular pages\n",
              conf.dsm_size);
      base_addr_ = (uint64_t)mmap(NULL, dsm_size_, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if ((void*)base_addr_ == MAP_FAILED) {
        perror("mmap fallback also failed");
        exit(1);
      }
    }

    // mbind 到指定的 data NUMA node
    if (numa_available() >= 0) {
      // 用 nodemask bitmask：支持 node ID > 63
      struct bitmask *nodemask = numa_bitmask_alloc(numa_num_possible_nodes());
      numa_bitmask_setbit(nodemask, data_node);
      int ret = mbind((void*)base_addr_, dsm_size_, MPOL_BIND,
                       nodemask->maskp, nodemask->size + 1,
                       MPOL_MF_MOVE | MPOL_MF_STRICT);
      numa_bitmask_free(nodemask);
      if (ret != 0) {
        perror("mbind data memory failed");
        fprintf(stderr, "Warning: data may not be on NUMA node %d\n", data_node);
      } else {
        printf("CXL DSMServer: data memory bound to NUMA node %d (%u GB)\n",
               data_node, conf.dsm_size);
      }
    }

    // ================================================================
    // 第二步：Warmup
    // ================================================================
    for (uint64_t i = 0; i < dsm_size_; i += 2 * define::MB) {
      *(char*)(base_addr_ + i) = 0;
    }
    memset((char*)base_addr_, 0, define::kChunkSize);

    // ================================================================
    // 第三步：Lock memory 在本地 NUMA node（模擬 on-chip）
    // ================================================================
    lock_pool_ = numa_available() >= 0
                     ? numa_alloc_onnode(define::kLockChipMemSize, lock_node)
                     : malloc(define::kLockChipMemSize);
    memset(lock_pool_, 0, define::kLockChipMemSize);
    printf("CXL DSMServer: lock memory on NUMA node %d (%lu KB)\n",
           lock_node, define::kLockChipMemSize / 1024);

    // ================================================================
    // 第四步：初始化 chunk allocator
    // ================================================================
    GlobalAddress dsm_start;
    dsm_start.nodeID = 0;
    dsm_start.offset = 0;
    chunk_alloc_ = new GlobalAllocator(dsm_start, dsm_size_);

    printf("CXL DSMServer: initialized with %u GB data memory\n",
           conf.dsm_size);
  }
};
