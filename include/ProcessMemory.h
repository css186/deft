#ifndef __PROCESS_MEMORY_H__
#define __PROCESS_MEMORY_H__

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>

struct ProcessMemoryStats {
  long vm_rss_kb = -1;
  long vm_hwm_kb = -1;
  long vm_size_kb = -1;
  long rss_anon_kb = -1;
  long hugetlb_pages_kb = -1;
};

inline ProcessMemoryStats read_process_memory_stats() {
  ProcessMemoryStats stats;

  std::ifstream status("/proc/self/status");
  std::string key;
  long value = 0;
  std::string unit;
  while (status >> key >> value >> unit) {
    if (key == "VmRSS:") {
      stats.vm_rss_kb = value;
    } else if (key == "VmHWM:") {
      stats.vm_hwm_kb = value;
    } else if (key == "VmSize:") {
      stats.vm_size_kb = value;
    } else if (key == "RssAnon:") {
      stats.rss_anon_kb = value;
    } else if (key == "HugetlbPages:") {
      stats.hugetlb_pages_kb = value;
    }
  }

  return stats;
}

inline void dump_process_memory_stats(const char *role, uint32_t id) {
  auto stats = read_process_memory_stats();
  printf(
      "[Process Memory] Role %s Id %u: VmRSS_kB=%ld VmHWM_kB=%ld "
      "VmSize_kB=%ld RssAnon_kB=%ld HugetlbPages_kB=%ld\n",
      role, id, stats.vm_rss_kb, stats.vm_hwm_kb, stats.vm_size_kb,
      stats.rss_anon_kb, stats.hugetlb_pages_kb);
  fflush(stdout);
}

#endif /* __PROCESS_MEMORY_H__ */
