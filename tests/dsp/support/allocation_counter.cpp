// Replaces the global operator new and delete for the DSP test executable,
// counting allocations on threads with an AllocationCounter alive. Built
// into anoesis_dsp_tests directly (not a library), so the replacement is
// always linked.

#include "allocation_counter.hpp"

#include <cstddef>
#include <cstdlib>
#include <new>

namespace
{

// Per-thread counting state: the replaced operator new is a free function,
// so its state has to be global.
thread_local bool counting = false;       // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
thread_local std::size_t allocations = 0; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

void* allocate(std::size_t size)
{
    if (counting)
    {
        ++allocations;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory): this *is* the allocator
    if (void* memory = std::malloc(size == 0 ? 1 : size))
    {
        return memory;
    }
    throw std::bad_alloc{};
}

void release(void* memory) noexcept
{
    std::free(memory); // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory): this *is* the allocator
}

} // namespace

namespace anoesis::test
{

AllocationCounter::AllocationCounter() noexcept : start(allocations), wasCounting(counting)
{
    counting = true;
}

AllocationCounter::~AllocationCounter()
{
    counting = wasCounting;
}

std::size_t AllocationCounter::count() const noexcept
{
    return allocations - start;
}

} // namespace anoesis::test

// The replaceable allocation functions. The nothrow and array forms are
// replaced too, so every unaligned allocation goes through allocate().
void* operator new(std::size_t size)
{
    return allocate(size);
}

void* operator new[](std::size_t size)
{
    return allocate(size);
}

void* operator new(std::size_t size, const std::nothrow_t& /*tag*/) noexcept
{
    try
    {
        return allocate(size);
    }
    catch (...)
    {
        return nullptr;
    }
}

void* operator new[](std::size_t size, const std::nothrow_t& /*tag*/) noexcept
{
    try
    {
        return allocate(size);
    }
    catch (...)
    {
        return nullptr;
    }
}

void operator delete(void* memory) noexcept
{
    release(memory);
}

void operator delete[](void* memory) noexcept
{
    release(memory);
}

void operator delete(void* memory, std::size_t /*size*/) noexcept
{
    release(memory);
}

void operator delete[](void* memory, std::size_t /*size*/) noexcept
{
    release(memory);
}

void operator delete(void* memory, const std::nothrow_t& /*tag*/) noexcept
{
    release(memory);
}

void operator delete[](void* memory, const std::nothrow_t& /*tag*/) noexcept
{
    release(memory);
}
