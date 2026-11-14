// Test support: counts heap allocations, to show that real-time code does not
// allocate.
//
// The DSP test executable replaces the global operator new
// (allocation_counter.cpp). While an AllocationCounter is alive, every
// operator new on its thread is counted. Check the count after the code
// under test and before any assertion: Catch2's assertions allocate.

#pragma once

#include <cstddef>

namespace anoesis::test
{

class AllocationCounter
{
public:
    AllocationCounter() noexcept;
    ~AllocationCounter();
    AllocationCounter(const AllocationCounter&) = delete;
    AllocationCounter& operator=(const AllocationCounter&) = delete;
    AllocationCounter(AllocationCounter&&) = delete;
    AllocationCounter& operator=(AllocationCounter&&) = delete;

    /// Allocations on this thread since construction.
    [[nodiscard]] std::size_t count() const noexcept;

private:
    std::size_t start;
    bool wasCounting;
};

} // namespace anoesis::test
