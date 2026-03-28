#include <iostream>
#include <tbb/global_control.h>
#include <tbb/info.h>
#include "Order.hpp"

int main()
{
    int num_threads = tbb::info::default_concurrency();
    int active_threads = std::max(1, num_threads - 1);

    tbb::global_control global_limit(
        tbb::global_control::max_allowed_parallelism,
        active_threads);

    std::cout << "--- OrderBook Engine --- \n";
    std::cout << "Worker Threads: " << active_threads << "\n";

    std::cout << "Memory Size of Order Struct: " << sizeof(Order) << " bytes\n";

    if (sizeof(Order) == 32)
    {
        std::cout << "Status: OPTIMIZED (Perfect 32-byte alignment)\n";
    }
    else
    {
        std::cout << "Status: WARNING (Unaligned struct size)\n";
    }

    return 0;
}