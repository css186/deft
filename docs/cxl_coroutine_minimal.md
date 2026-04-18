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
I/O-overlap mechanism into a conservative cooperative scheduler that is safe for
shared-memory execution.

## What Was Changed

All changes are localized to [src/Tree.cpp](../src/Tree.cpp).

### 1. Replaced the RDMA CQ-driven coroutine scheduler with round-robin for CXL

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

### 2. Added a small worker time slice

In `coro_worker()`, the CXL path now yields back to the master after a small
number of completed operations. The current slice length is:

- `kCxlCoroOpsPerSlice = 8`

This prevents one coroutine from running an arbitrarily long streak of
uncontended operations while the others remain unscheduled. The slice is short
enough to keep the round-robin scheduler active, but long enough to avoid
switching on every single request.

### 3. Kept lock/retry logic unchanged

An earlier draft attempted to yield directly inside lock acquisition and CAS
retry loops. That approach is unsafe in the current DEFT locking model because a
coroutine may already hold a lock, hold a shared lock during an upgrade path, or
already occupy a position in the SX lock ticket sequence when the yield occurs.

Under cooperative scheduling, yielding in those states can artificially extend
lock hold time or delay ticket progress, which is much more likely to cause
flaky stalls than to improve throughput.

For that reason, the minimal implementation intentionally does **not** yield
inside:

- `acquire_sx_lock()`
- `lock_and_read()`
- leaf CAS retry loops

This keeps the synchronization semantics identical to the original code and
limits coroutine-specific changes to the scheduler itself.

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

- provide basic fairness across coroutines on the same worker thread
- avoid the RDMA-only assumption that progress comes from CQ completions
- offer a minimal cooperative scheduling baseline for CXL experiments

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

- A runnable queue / blocked queue design instead of simple round-robin.
- Lock-address-aware scheduling so coroutines blocked on the same hot page do
  not immediately contend again.
- Safe yield points that are explicitly aware of lock ownership, SX ticket
  state, and upgrade state.
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
