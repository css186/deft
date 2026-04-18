/**
 * dsm_client_cxl.cpp
 * 
 * 為什麼需要這個 .cpp 檔？
 * 因為 DSMClient 有幾個 thread_local static 成員變數，
 * C++ 要求它們在一個 .cpp 檔中定義（不能只在 header 中宣告）。
 * 
 * 也包含 RegisterThread() 的實作，這個方法初始化每個線程的 buffer。
 */
#include "cxl/dsm_client_cxl.h"

// ================================================================
// 原本定義在 Directory.cpp 中的全域變數
// CXL build 不編譯 Directory.cpp，所以需要在這裡提供定義
// Tree.cpp 通過 extern 引用這些變數
// ================================================================
std::atomic<uint64_t> g_root_ptr_raw{GlobalAddress::Null().raw};
int g_root_level = -1;
bool enable_cache = true;

// thread_local 變數定義
// 每個 thread 有自己的 ID、buffer、allocator
thread_local int DSMClient::thread_id_ = -1;
thread_local char *DSMClient::rdma_buffer_ = nullptr;
thread_local LocalAllocator DSMClient::local_allocator_;
thread_local RdmaBuffer DSMClient::rbuf_[define::kMaxCoro];
thread_local uint64_t DSMClient::thread_tag_ = 0;

/**
 * RegisterThread() - 為當前線程分配資源
 * 
 * 原版做什麼？
 *   1. 分配 thread_id
 *   2. 綁定到對應的 ThreadConnection (QP/CQ)
 *   3. 初始化 RPC message 的 recv/send buffer
 *   4. 設定 RDMA buffer (在 registered memory 中)
 * 
 * CXL 版本只需要 #1 和 #4：
 *   - 不需要 ThreadConnection (沒有 QP/CQ)
 *   - 不需要 RPC message buffer
 *   - buffer 仍然需要（Tree.cpp 用 RdmaBuffer 來管理 page_buffer、cas_buffer 等）
 */
void DSMClient::RegisterThread() {
  if (thread_id_ != -1) return;  // 已經註冊過了

  thread_id_ = app_id_.fetch_add(1);
  thread_tag_ = thread_id_ + (((uint64_t)get_my_client_id()) << 32) + 1;

  // 分配 buffer 空間
  // 原版中 buffer 在 RDMA registered memory (cache_.data) 中
  // CXL 版本中不需要 register，但仍然可以用 cache_.data 作為 buffer pool
  rdma_buffer_ = (char *)cache_.data + thread_id_ * define::kPerThreadRdmaBuf;

  for (int i = 0; i < define::kMaxCoro; ++i) {
    rbuf_[i].set_buffer(rdma_buffer_ + i * define::kPerCoroRdmaBuf);
  }

  printf("CXL DSMClient: thread %d registered (tag=%lu)\n",
         thread_id_, thread_tag_);
}
