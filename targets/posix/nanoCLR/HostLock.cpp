//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

// The lock that keeps the interpreter and the Wire Protocol thread out of each
// other's way. See the comment on NANOCLR_HOST_LOCK_ACQUIRE in Execution.cpp
// for why it exists and where it is taken.
//
// The two sides are not symmetric, and that asymmetry is the whole point. The
// interpreter takes and drops the lock once per scheduling batch, thousands of
// times a second; the debugger takes it once per command. A plain mutex gives
// no fairness, so the interpreter simply reacquires before a waiting debugger
// thread is ever scheduled - measured: every connect attempt failed, 0 of 12,
// because the ping reply never got the lock. Hence the waiter count: when
// somebody is queued, the interpreter stands aside for a moment after releasing
// instead of racing straight back in.
//
// Deliberately not recursive. Neither side nests, and a plain mutex turns a
// future nesting mistake into a deadlock that shows up at once rather than into
// a lock that quietly protects nothing.

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace
{
std::mutex s_hostLock;

// how many debugger commands are queued for the lock
std::atomic<int> s_waiting{0};
} // namespace

extern "C" void NanoCLR_HostLock_AcquireForInterpreter()
{
    s_hostLock.lock();
}

extern "C" void NanoCLR_HostLock_ReleaseForInterpreter()
{
    s_hostLock.unlock();

    // Yield is not enough here: the interpreter is runnable and the waiter may
    // be on another core, so it can win the reacquire race repeatedly. A short
    // sleep is a real concession, and it is only paid while a command is
    // actually queued, which outside a debugger session is never.
    if (s_waiting.load(std::memory_order_relaxed) > 0)
    {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

extern "C" void NanoCLR_HostLock_AcquireForDebugger()
{
    s_waiting.fetch_add(1, std::memory_order_relaxed);
    s_hostLock.lock();
    s_waiting.fetch_sub(1, std::memory_order_relaxed);
}

extern "C" void NanoCLR_HostLock_ReleaseForDebugger()
{
    s_hostLock.unlock();
}
