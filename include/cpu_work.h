#pragma once
#include <cstddef>
#include <functional>

// A bounded, shared pool. Small ranges execute inline; large ranges share work
// without creating threads per operation or waiting for queued helper jobs.
void enqueueCpuWork(std::function<void()> work);
// Serial document-journal I/O stays separate from CPU decoding/analysis work.
void enqueueIoWork(std::function<void()> work);
void parallelCpuRange(std::size_t count, std::size_t grain, const std::function<void(std::size_t, std::size_t)> &work);
void waitForCpuWork();
unsigned cpuWorkerCount();
