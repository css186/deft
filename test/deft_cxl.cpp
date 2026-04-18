#ifdef __linux__
#define _GNU_SOURCE
#endif

/**
 * deft_cxl.cpp - CXL 版本的 benchmark 入口
 * 
 * 這個檔案等同於原版的 server.cpp + client.cpp 合體。
 * 
 * 原版流程：
 *   1. 先啟動 ./server（在 server 機器上，分配記憶體、等待 client 連接）
 *   2. 再啟動 ./client（在 client 機器上，通過 RDMA 連接 server、跑 benchmark）
 * 
 * CXL 版本流程：
 *   1. 分配記憶體（NUMA node 1）— 原來 server 做的事
 *   2. 初始化 DSMClient — 原來 client 做的事
 *   3. 跑 benchmark — 和原版 client.cpp 完全一樣
 * 
 * 全部在一個進程中完成，因為 CXL 是同一台機器的 memory。
 */
#define STRIP_FLAG_HELP 1
#include <gflags/gflags.h>
#include "Timer.h"
#include "Tree.h"
#include "zipf.h"

// CXL specific includes
#include "cxl/dsm_server_cxl.h"

#include <city.h>
#include <execinfo.h>
#include <exception>
#include <signal.h>
#include <stdlib.h>
#include <chrono>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>
#include <random>

// =================== Workload Parameters ===================
// 和原版 client.cpp 完全一樣

// #define USE_CORO
const int kCoroCnt = 3;

DEFINE_int32(num_prefill_threads, 1, "prefill thread");
DEFINE_int32(num_bench_threads, 1, "bench thread");
DEFINE_int32(read_ratio, 50, "read ratio");
DEFINE_int32(key_space, 200 * 1e6, "key space");
DEFINE_int32(ops_per_thread, 10 * 1e6, "ops per thread");
DEFINE_double(prefill_ratio, 1, "prefill ratio");
DEFINE_double(zipf, 0.99, "zipf");
DEFINE_int32(dsm_size, 32, "DSM size in GB");
DEFINE_int32(data_numa, 8, "NUMA node for data memory (CXL memory, should be on remote socket)");
DEFINE_int32(lock_numa, 0, "NUMA node for lock memory (on-chip, should be on local socket)");

constexpr int NUM_WARMUP_OPS = 1e6;
constexpr int MEASURE_SAMPLE = 32;

// =================== Global Variables ===================

std::thread th[MAX_APP_THREAD];
uint64_t total_time[MAX_APP_THREAD][8];
double prefill_tp = 0.;
double prefill_lat = 0.;
int MAX_TOTAL_THREADS = 0;
std::atomic<uint64_t> prefill_ops_done{0};
uint64_t prefill_target_ops = 0;

Tree *tree;
DSMClient *dsm_client;

namespace {

[[noreturn]] void cxl_fatal_signal_handler(int sig) {
  void *frames[64];
  int n = backtrace(frames, 64);
  fprintf(stderr, "\n[FATAL] received signal %d\n", sig);
  backtrace_symbols_fd(frames, n, STDERR_FILENO);
  fflush(stderr);
  _exit(128 + sig);
}

[[noreturn]] void cxl_terminate_handler() {
  fprintf(stderr, "\n[FATAL] std::terminate invoked\n");
  if (auto ep = std::current_exception()) {
    try {
      std::rethrow_exception(ep);
    } catch (const std::exception &e) {
      fprintf(stderr, "[FATAL] uncaught exception: %s\n", e.what());
    } catch (...) {
      fprintf(stderr, "[FATAL] uncaught non-std exception\n");
    }
  } else {
    fprintf(stderr, "[FATAL] no active exception\n");
  }
  void *frames[64];
  int n = backtrace(frames, 64);
  backtrace_symbols_fd(frames, n, STDERR_FILENO);
  fflush(stderr);
  abort();
}

void install_fatal_handlers() {
  std::set_terminate(cxl_terminate_handler);
  signal(SIGABRT, cxl_fatal_signal_handler);
  signal(SIGSEGV, cxl_fatal_signal_handler);
  signal(SIGBUS, cxl_fatal_signal_handler);
  signal(SIGILL, cxl_fatal_signal_handler);
  signal(SIGFPE, cxl_fatal_signal_handler);
}

}  // namespace

inline Key to_key(uint64_t k) {
  return (CityHash64((char *)&k, sizeof(k))) % FLAGS_key_space + 1;
}

// Coroutine request generator (和原版一樣)
class RequsetGenBench : public RequstGen {
 public:
  RequsetGenBench(int coro_id, DSMClient *dsm_client, int thread_id)
      : coro_id(coro_id), dsm_client_(dsm_client), thread_id_(thread_id) {
    seed = (dsm_client->get_my_client_id() << 10) + (thread_id_ << 5) + coro_id;
    mehcached_zipf_init(&state, FLAGS_key_space, FLAGS_zipf, seed);
  }

  Request next() override {
    Request r;
    r.is_search = rand_r(&seed) % 100 < FLAGS_read_ratio;
    uint64_t dis = mehcached_zipf_next(&state);
    r.k = to_key(dis);
    r.v = 23 + dis + seed;
    return r;
  }

 private:
  int coro_id;
  DSMClient *dsm_client_;
  int thread_id_;
  unsigned int seed;
  struct zipf_gen_state state;
};

RequstGen *coro_func(int coro_id, DSMClient *dsm_client, int id) {
  return new RequsetGenBench(coro_id, dsm_client, id);
}

// =================== Benchmark Thread ===================
// 除了去掉 RDMA 相關的 barrier，和原版 client.cpp 基本一樣。

std::atomic<int64_t> prefill_cnt{0};

void thread_run(int id) {
  dsm_client->RegisterThread();

  uint64_t my_id = MAX_TOTAL_THREADS * dsm_client->get_my_client_id() + id;
  printf("I am thread %ld on compute nodes\n", my_id);
  uint64_t all_prefill_threads = FLAGS_num_prefill_threads;

  Timer timer;
  Timer total_timer;

  // ---- Prefill ----
  if (id < FLAGS_num_prefill_threads) {
    uint64_t end_prefill_key = FLAGS_prefill_ratio * FLAGS_key_space;
    uint64_t begin_prefill_key = my_id;
    std::vector<uint64_t> prefill_keys;
    prefill_keys.reserve(
        (end_prefill_key - begin_prefill_key) / all_prefill_threads + 1);
    for (uint64_t i = begin_prefill_key; i < end_prefill_key;
         i += all_prefill_threads) {
      prefill_keys.push_back(i + 1);
    }

    std::shuffle(prefill_keys.begin(), prefill_keys.end(),
                 std::default_random_engine(my_id));
    total_timer.begin();
    uint64_t local_progress = 0;
    for (size_t i = 0; i < prefill_keys.size(); ++i) {
      bool measure_lat = i % MEASURE_SAMPLE == 0;
      if (measure_lat) {
        timer.begin();
      }
      tree->insert(to_key(prefill_keys[i]), prefill_keys[i] * 2);
      if (measure_lat) {
        auto t = timer.end();
        stat_helper.add(id, lat_op, t);
      }
      if (++local_progress == 4096) {
        prefill_ops_done.fetch_add(local_progress, std::memory_order_relaxed);
        local_progress = 0;
      }
    }
    if (local_progress != 0) {
      prefill_ops_done.fetch_add(local_progress, std::memory_order_relaxed);
    }
    total_time[id][0] = total_timer.end();
    total_time[id][1] = prefill_keys.size();
  }
  prefill_cnt.fetch_add(1);

  if (id == 0) {
    auto start = std::chrono::steady_clock::now();
    auto next_report = start + std::chrono::seconds(5);
    while (prefill_cnt.load() != MAX_TOTAL_THREADS) {
      auto now = std::chrono::steady_clock::now();
      if (now >= next_report) {
        uint64_t done = prefill_ops_done.load(std::memory_order_relaxed);
        double pct = prefill_target_ops == 0
                         ? 0.0
                         : 100.0 * (double)done / (double)prefill_target_ops;
        double elapsed =
            std::chrono::duration<double>(now - start).count();
        printf("[prefill] %.1fs %lu / %lu (%.1f%%)\n", elapsed, done,
               prefill_target_ops, pct);
        fflush(stdout);
        next_report = now + std::chrono::seconds(5);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    printf("prefill time %lds\n", total_time[0][0] / 1000 / 1000 / 1000);
    prefill_tp = 0.;
    uint64_t hit = 0;
    uint64_t all = 0;
    for (int i = 0; i < FLAGS_num_prefill_threads; ++i) {
      prefill_tp += (double)total_time[i][1] * 1e3 / total_time[i][0];
      hit += cache_hit[i][0];
      all += (cache_hit[i][0] + cache_miss[i][0]);
    }
    uint64_t stat_lat[lat_end];
    uint64_t stat_cnt[lat_end];
    for (int k = 0; k < lat_end; k++) {
      stat_lat[k] = 0;
      stat_cnt[k] = 0;
      for (int i = 0; i < MAX_APP_THREAD; ++i) {
        stat_lat[k] += stat_helper.latency_[i][k];
        stat_helper.latency_[i][k] = 0;
        stat_cnt[k] += stat_helper.counter_[i][k];
        stat_helper.counter_[i][k] = 0;
      }
    }
    prefill_lat = (double)stat_lat[lat_op] / stat_cnt[lat_op];
    printf("Load Tp %.4lf Mops/s Lat %.1lf\n", prefill_tp, prefill_lat);
    printf("cache hit rate: %.1lf%%\n", hit * 100.0 / all);
    fflush(stdout);

    tree->index_cache_statistics();
    tree->clear_statistics();
    memset(reinterpret_cast<void *>(&stat_helper), 0, sizeof(stat_helper));

    prefill_cnt.store(0);
    prefill_ops_done.store(0, std::memory_order_relaxed);
  }

  while (prefill_cnt.load() != 0)
    ;

  // ---- Benchmark ----
  if (id >= FLAGS_num_bench_threads) {
    return;
  }

#ifdef USE_CORO
  bool lock_bench = false;
  tree->run_coroutine(coro_func, id, kCoroCnt, lock_bench, NUM_WARMUP_OPS);
  total_timer.begin();
  tree->run_coroutine(coro_func, id, kCoroCnt, lock_bench,
                      FLAGS_ops_per_thread);
  total_time[id][0] = total_timer.end();
#else
  uint32_t seed = (dsm_client->get_my_client_id() << 10) + id;
  struct zipf_gen_state state;
  mehcached_zipf_init(&state, FLAGS_key_space, FLAGS_zipf, seed);

  // warmup
  for (int i = 0; i < NUM_WARMUP_OPS; ++i) {
    uint64_t dis = mehcached_zipf_next(&state);
    uint64_t key = to_key(dis);
    Value v;
    if (rand_r(&seed) % 100 < FLAGS_read_ratio) {
      tree->search(key, v);
    } else {
      v = 12;
      tree->insert(key, v);
    }
  }

  total_timer.begin();
  for (int i = 0; i < FLAGS_ops_per_thread; ++i) {
    uint64_t dis = mehcached_zipf_next(&state);
    uint64_t key = to_key(dis);

    bool measure_lat = i % MEASURE_SAMPLE == 0;
    if (measure_lat) {
      timer.begin();
    }

    Value v;
    if (rand_r(&seed) % 100 < FLAGS_read_ratio) {
      tree->search(key, v);
    } else {
      v = 12;
      tree->insert(key, v);
    }

    if (measure_lat) {
      auto t = timer.end();
      stat_helper.add(id, lat_op, t);
    }
  }
  total_time[id][0] = total_timer.end();
#endif
}

// =================== Main ===================

void print_args() {
  printf(
      "[CXL Mode] DSMSize %dGB, DataNUMA %d, LockNUMA %d, "
      "PrefillThreads %d, BenchThreads %d, "
      "ReadRatio %d, KeySpace %d, Zipfan %.3lf\n",
      FLAGS_dsm_size, FLAGS_data_numa, FLAGS_lock_numa,
      FLAGS_num_prefill_threads, FLAGS_num_bench_threads,
      FLAGS_read_ratio, FLAGS_key_space, FLAGS_zipf);
}

int main(int argc, char *argv[]) {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  setvbuf(stderr, nullptr, _IOLBF, 0);
  install_fatal_handlers();
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  print_args();

  MAX_TOTAL_THREADS =
      std::max(FLAGS_num_prefill_threads, FLAGS_num_bench_threads);
  prefill_target_ops = (uint64_t)(FLAGS_prefill_ratio * FLAGS_key_space);
  if (MAX_TOTAL_THREADS > MAX_APP_THREAD) {
    fprintf(stderr,
            "CXL benchmark requires %d threads, but MAX_APP_THREAD=%d\n",
            MAX_TOTAL_THREADS, MAX_APP_THREAD);
    return 1;
  }

  // ===========================================================
  // Step 1: 初始化 Server（分配 CXL 記憶體）
  // 原版中這是在另一台機器上的獨立進程
  // ===========================================================
  DSMConfig server_config;
  server_config.dsm_size = FLAGS_dsm_size;
  DSMServer *server = DSMServer::GetInstance(server_config,
                                              FLAGS_data_numa,
                                              FLAGS_lock_numa);
  printf("=== CXL Memory Server initialized ===\n");

  // ===========================================================
  // Step 2: 初始化 Client
  // ===========================================================
  DSMConfig client_config;
  uint64_t required_rbuf_bytes =
      (uint64_t)MAX_TOTAL_THREADS * define::kPerThreadRdmaBuf;
  client_config.cache_size =
      std::max<uint32_t>(1, (required_rbuf_bytes + define::GB - 1) / define::GB);
  printf("CXL DSMClient: allocating %u GB local buffer cache for %d threads\n",
         client_config.cache_size, MAX_TOTAL_THREADS);
  dsm_client = DSMClient::GetInstance(client_config, server);

  // 主線程先註冊
  dsm_client->RegisterThread();

  // ===========================================================
  // Step 3: 初始化 Tree
  // 這和原版完全一樣
  // ===========================================================
  tree = new Tree(dsm_client);

  // 初始插入一些 keys
  tree->insert(to_key(0), 1);
  for (uint64_t i = 1; i < 1000000; ++i) {
    tree->insert(to_key(i), i * 2);
  }
  printf("=== Initial insertion complete ===\n");

  // ===========================================================
  // Step 4: 啟動 benchmark 線程
  // ===========================================================
  for (int i = 1; i < MAX_TOTAL_THREADS; i++) {
    th[i] = std::thread(thread_run, i);
  }
  thread_run(0);

  for (int i = 1; i < MAX_TOTAL_THREADS; i++) {
    if (th[i].joinable()) {
      th[i].join();
    }
  }
  delete tree;

  // ===========================================================
  // Step 5: 輸出結果
  // ===========================================================
  double all_tp = 0.;
  uint64_t hit = 0;
  uint64_t all = 0;
  for (int i = 0; i < FLAGS_num_bench_threads; ++i) {
    all_tp += (double)FLAGS_ops_per_thread * 1e3 / total_time[i][0];
    hit += cache_hit[i][0];
    all += (cache_hit[i][0] + cache_miss[i][0]);
  }

  uint64_t stat_lat[lat_end];
  uint64_t stat_cnt[lat_end];
  for (int k = 0; k < lat_end; k++) {
    stat_lat[k] = 0;
    stat_cnt[k] = 0;
    for (int i = 0; i < MAX_APP_THREAD; ++i) {
      stat_lat[k] += stat_helper.latency_[i][k];
      stat_cnt[k] += stat_helper.counter_[i][k];
    }
  }

  printf("\n========== CXL Benchmark Results ==========\n");
  printf("Loading Results: TP %.3lf Mops/s, Lat %.3lf us\n",
         prefill_tp, prefill_lat / 1000.0);
  printf("Benchmark TP: %.3lf Mops/s\n", all_tp);
  printf("Avg op latency: %.1lf ns\n",
         (double)stat_lat[lat_op] / stat_cnt[lat_op]);
  printf("Cache hit rate: %.1lf%%\n", hit * 100.0 / all);
  printf("Avg lock latency: %.1lf ns\n",
         (double)stat_lat[lat_lock] / stat_cnt[lat_lock]);
  printf("Avg read page latency: %.1lf ns\n",
         (double)stat_lat[lat_read_page] / stat_cnt[lat_read_page]);
  printf("Avg write page latency: %.1lf ns\n",
         (double)stat_lat[lat_write_page] / stat_cnt[lat_write_page]);
  printf("Final Results: TP %.3lf Mops/s, Lat %.3lf us\n",
         all_tp, (double)stat_lat[lat_op] / stat_cnt[lat_op] / 1000.0);
  printf("============================================\n");

  return 0;
}
