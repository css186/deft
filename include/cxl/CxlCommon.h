/**
 * CxlCommon.h - CXL 版本的 Common 定義
 *
 * 策略：
 *   1. 先 define Rdma.h 的 header guard (_RDMA_H__)，阻止原版 Rdma.h 被 include
 *      （因為 Rdma.h include <infiniband/verbs.h>，CXL 環境沒有這個 header）
 *   2. 提供 Rdma.h 中我們需要的定義（RdmaOpRegion, kOroMax）
 *   3. 然後 include 原版 Common.h — 它看到 _RDMA_H__ 已定義，就跳過 Rdma.h
 *
 * 這樣所有 Tree.cpp 需要的定義都來自原版 Common.h，
 * 不會有重複定義的問題。
 */
#pragma once

// ================================================================
// Step 1: Block Rdma.h from being included (it needs ibverbs)
// ================================================================
#ifndef _RDMA_H__
#define _RDMA_H__

// Step 2: Provide the definitions from Rdma.h that Tree.cpp needs
#include "Debug.h"  // Rdma.h includes Debug.h, so we need it too

constexpr int kOroMax = 3;

struct RdmaOpRegion {
  uint64_t source;
  uint64_t dest;
  union {
    uint64_t size;
    uint64_t log_sz;  // used for extended atomic
  };

  uint32_t lkey;
  union {
    uint32_t remoteRKey;
    bool is_on_chip;
  };
};

extern int kMaxDeviceMemorySize;

#endif  // _RDMA_H__

// ================================================================
// Step 3: Now include the original Common.h
// It will #include "Rdma.h" but that will be a no-op because
// _RDMA_H__ is already defined.
// ================================================================
#include "Common.h"
