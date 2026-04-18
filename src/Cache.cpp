#include "Cache.h"
#include "Debug.h"

#include <sys/mman.h>
#include <cstdlib>

Cache::Cache(const CacheConfig &cache_config) {
    size = cache_config.cacheSize;
    size_t bytes = (size_t)size * define::GB;
    void *buf = hugePageAlloc(bytes);

#ifdef DEFT_CXL
    if (buf == MAP_FAILED) {
        Debug::notifyInfo("CXL cache hugepage alloc failed, falling back to regular pages");
        buf = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (buf == MAP_FAILED) {
            Debug::notifyError("CXL cache regular mmap failed");
            std::abort();
        }
#ifdef MADV_HUGEPAGE
        madvise(buf, bytes, MADV_HUGEPAGE);
#endif
    }
#else
    if (buf == MAP_FAILED) {
        Debug::notifyError("cache hugepage alloc failed");
        std::abort();
    }
#endif

    data = (uint64_t)buf;
}
