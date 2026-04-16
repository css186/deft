#include "Directory.h"
#include "Common.h"
#include "ProcessMemory.h"

#include "connection.h"
#include "DirectoryConnection.h"

GlobalAddress g_root_ptr = GlobalAddress::Null();
int g_root_level = -1;
bool enable_cache = true;

Directory::Directory(DirectoryConnection *dCon, uint16_t dirID, uint16_t nodeID)
    : dCon(dCon), dirID(dirID), nodeID(nodeID), dirTh(nullptr) {
  {  // chunck alloctor
    GlobalAddress dsm_start;
    uint64_t per_directory_dsm_size = dCon->dsmSize / NR_DIRECTORY;
    dsm_start.nodeID = nodeID;
    dsm_start.offset = per_directory_dsm_size * dirID;
    chunckAlloc = new GlobalAllocator(dsm_start, per_directory_dsm_size);
  }

  // dirTh = new std::thread(&Directory::dirThread, this);
}

Directory::~Directory() { delete chunckAlloc; }

void Directory::dirThread() {

  // bindCore(35 - dirID);
  Debug::notifyInfo("thread %d in memory nodes runs...\n", dirID);

  while (!stop_flag.load(std::memory_order_acquire)) {
    struct ibv_wc wc;
    pollWithCQ(dCon->cq, 1, &wc);

    switch (int(wc.opcode)) {
    case IBV_WC_RECV: // control message
    {

      auto *m = (RawMessage *)dCon->message->getMessage();

      process_message(m);

      break;
    }
    case IBV_WC_RDMA_WRITE: {
      break;
    }
    case IBV_WC_RECV_RDMA_WITH_IMM: {

      break;
    }
    default:
      assert(false);
    }
  }
}

void Directory::process_message(const RawMessage *m) {

  RawMessage *send = nullptr;
  switch (m->type) {
  case RpcType::MALLOC: {
    send = (RawMessage*)dCon->message->getSendPool();
    send->addr = chunckAlloc->alloc_chunck();
    
    malloc_count_++;
    break;
  }

  case RpcType::NEW_ROOT: {

    if (g_root_level < m->level) {
      g_root_ptr = m->addr;
      g_root_level = m->level;
      if (g_root_level >= 3) {
        enable_cache = true;
      }
    }

    break;
  }

  case RpcType::TERMINATE: {
    dump_memory_stats();
    stop_flag.store(true, std::memory_order_release);
    break;
  }

  default:
    assert(false);
  }

  if (send) {
    dCon->sendMessage2App(send, m->node_id, m->app_id);
  }
}

void Directory::dump_memory_stats() {
  if (!chunckAlloc) return;

  size_t used_chunks = chunckAlloc->get_used_chunks();
  size_t usable_chunks = chunckAlloc->get_usable_chunks();
  size_t used_bytes = used_chunks * define::kChunkSize;
  size_t usable_bytes = usable_chunks * define::kChunkSize;
  double util = usable_chunks == 0
                    ? 0.0
                    : static_cast<double>(used_chunks) / usable_chunks * 100.0;

  printf(
      "[Memory Utilization] Node %d Dir %d: %.2f%% "
      "used_bytes=%zu usable_bytes=%zu used_chunks=%zu usable_chunks=%zu "
      "chunk_bytes=%llu mallocs=%zu\n",
      nodeID, dirID, util, used_bytes, usable_bytes, used_chunks, usable_chunks,
      static_cast<unsigned long long>(define::kChunkSize), malloc_count_);
  dump_process_memory_stats("server", nodeID);

  fflush(stdout);
}
