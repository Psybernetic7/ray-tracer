#ifndef PARALLEL_H
#define PARALLEL_H

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>


// Calls body(index) once for every index in [0, count), spread over a pool of threads.
//
// Threads claim the next index from a shared counter, the same scheme the renderer uses for
// tiles, so uneven work (a valley cell that needs far more rays than a peak) balances itself.
// thread_count 0 means one thread per hardware thread. The calling thread does its share.
template <class Body>
void parallel_for(int count, int thread_count, Body&& body) {
    if (count <= 0)
        return;

    std::atomic<int> next{0};
    auto worker = [&]() {
        for (int index = next++; index < count; index = next++)
            body(index);
    };

    int threads = thread_count > 0 ? thread_count : int(std::thread::hardware_concurrency());
    threads = std::clamp(threads, 1, count);

    std::vector<std::thread> helpers;
    for (int t = 1; t < threads; t++)
        helpers.emplace_back(worker);
    worker();
    for (auto& helper : helpers)
        helper.join();
}

#endif
