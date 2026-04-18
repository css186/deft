# Minimal Shared-Memory Coroutine Adaptation for CXL

## Background

The original DEFT coroutine design targets RDMA. In the RDMA path, a coroutine
submits an asynchronous DSM request, yields, and later resumes after the NIC
completion queue reports that the request has finished. The benefit comes from
hiding remote-memory latency with useful work from other coroutines.

This assumption no longer holds in the CXL single-machine mode. In the CXL
client implementation, DSM operations are executed synchronously with local
shared-memory accesses, so there is no real "request in flight" interval for
the coroutine scheduler to overlap. If the original RDMA scheduler is kept
unchanged, coroutine switching mostly adds scheduling overhead without gaining
latency hiding.

Therefore, the CXL conversion needs a different coroutine objective. Instead of
hiding network latency, coroutines should only help when a thread is wasting CPU
time in lock contention or repeated retry loops.

## Design Goal

This change implements a minimal CXL-specific coroutine mode with the following
constraints:

- Keep DEFT's data structure and lock protocol unchanged.
- Avoid redesigning the full coroutine framework.
- Only introduce yielding at clear contention points.
- Preserve the original RDMA coroutine implementation for non-CXL builds.

The goal is not to build a full shared-memory task scheduler. The goal is only
to make `USE_CORO` meaningful under `DEFT_CXL` by converting it from an
I/O-overlap mechanism into a contention-relief mechanism.

## What Was Changed

All changes are localized to [src/Tree.cpp](../src/Tree.cpp).

### 1. Added a CXL-specific yield policy

Two helper functions were added near the top of `Tree.cpp`:

- `cxl_coro_should_yield(uint64_t retry_cnt)`
- `cxl_coro_yield(CoroContext *ctx)`

The first helper decides when a retry loop should yield. The current policy is
intentionally simple: yield once every 16 retries. This keeps the implementation
small and avoids yielding on every transient retry.

The second helper performs a coroutine yield only when a coroutine context is
present. This keeps the normal non-coroutine path unchanged.

### 2. Added yielding to lock contention paths

The original RDMA version spins in retry loops while waiting for lock state to
become compatible. Under CXL, this becomes local busy waiting. To avoid wasting
an entire worker thread on one hot lock, the CXL coroutine path now yields from
these retry loops after repeated failures:

- `acquire_sx_lock()`
- `lock_and_read()`

The lock protocol itself is unchanged. The thread still retries the same lock
operation; the only difference is that after enough retries it temporarily gives
control to another coroutine on the same worker thread.

### 3. Added yielding to leaf CAS retry paths

The leaf insertion path already has retry loops when a compare-and-swap loses a
race. In the CXL mode, these retries also become pure local spinning. To avoid
burning CPU on a hot leaf page, the following retry points now yield after
repeated failures:

- `retry_insert`
- `retry_insert_2`

Again, this does not change the insertion algorithm. It only changes how long a
single coroutine monopolizes the worker thread while retrying.

### 4. Replaced the RDMA CQ-driven coroutine scheduler with round-robin for CXL

The original `coro_master()` is driven by `PollRdmaCqOnce()`. That makes sense
for RDMA because workers resume when outstanding requests complete.

Under CXL, there is no equivalent asynchronous completion source. Therefore, the
minimal adaptation replaces the master scheduler with a simple round-robin loop
when `DEFT_CXL` is enabled:

- iterate over all workers
- resume each worker once
- repeat until all operations finish

This is a deliberately small change. It avoids pretending that CXL has a
completion queue while still preserving the existing coroutine framework.

### 5. Added a small worker time slice

In `coro_worker()`, the CXL path now yields back to the master after a small
number of completed operations. The current slice length is:

- `kCxlCoroOpsPerSlice = 8`

This prevents one coroutine from running an arbitrarily long streak of
uncontended operations while the others remain unscheduled. The slice is short
enough to keep the round-robin scheduler active, but long enough to avoid
switching on every single request.

## Why This Is a Minimal Change

This implementation intentionally does **not**:

- add a new scheduler data structure
- add runnable queues or blocked queues
- change the DEFT lock format
- change the tree traversal algorithm
- change page layout or cache policy
- alter the original RDMA coroutine code path

Only the scheduling policy under `DEFT_CXL` is adjusted, and only in places
where synchronous shared-memory execution makes the RDMA logic unsuitable.

## Expected Behavior

With this change, coroutines under CXL are no longer expected to hide memory
latency. Instead, they are expected to:

- reduce wasted CPU spinning during lock contention
- let another request make progress while one coroutine is stuck retrying
- provide a basic fairness improvement under hot-page contention

This means the CXL coroutine mode should be interpreted differently from the
RDMA coroutine mode:

- **RDMA**: latency hiding for asynchronous remote requests
- **CXL**: cooperative scheduling for synchronous contention paths

## Tradeoff

This minimal version favors simplicity over optimality.

The round-robin scheduler and fixed retry threshold are easy to reason about,
but they are not necessarily optimal for every workload. In low-contention
cases, coroutine switching may still add overhead. In high-contention cases, the
benefit depends on whether yielding allows another coroutine to make useful
progress on a different key or page.

That tradeoff is acceptable here because the purpose of this patch is to
introduce a CXL-appropriate baseline with minimal structural change.

## Future Work

The following items were intentionally left out of the minimal implementation
and should be treated as future work:

- Adaptive yield thresholds based on retry count, lock hotness, or observed
  contention.
- A runnable queue / blocked queue design instead of simple round-robin.
- Lock-address-aware scheduling so coroutines blocked on the same hot page do
  not immediately contend again.
- Backoff strategies that combine yielding with contention-aware delay control.
- Yielding in additional retry-heavy paths such as cache-miss retries or root
  retry loops, after separate evaluation.
- Per-workload tuning of the operation slice length instead of a fixed constant.
- A dedicated shared-memory coroutine scheduler decoupled from the original RDMA
  coroutine control flow.

## Summary

This change adapts DEFT's coroutine support to the CXL shared-memory model in
the smallest possible way. The original RDMA coroutine mechanism depends on
asynchronous remote completions, which do not exist in the CXL implementation.
The new CXL path therefore reinterprets coroutines as a cooperative mechanism
for relieving lock contention and retry spinning, while keeping the original
data structure and concurrency design intact.
