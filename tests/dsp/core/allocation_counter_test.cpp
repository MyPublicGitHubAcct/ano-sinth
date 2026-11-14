// Tests for the allocation counter that the real-time tests use to show that
// code under test does not allocate.

#include "../support/allocation_counter.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <vector>

using anoesis::test::AllocationCounter;

TEST_CASE("AllocationCounter counts heap allocations made while it is alive", "[test-support][allocation]")
{
    std::size_t counted = 0;
    {
        const AllocationCounter counter;
        const auto one = std::make_unique<int>(1);
        const std::vector<float> many(100);
        counted = counter.count();
        static_cast<void>(one);
        static_cast<void>(many);
    }
    CHECK(counted == 2);

    std::size_t none = 1;
    {
        const AllocationCounter counter;
        const int onStack = 3;
        static_cast<void>(onStack);
        none = counter.count();
    }
    CHECK(none == 0);
}
