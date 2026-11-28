// A PC D3D12_DESCRIPTOR_HEAP_DESC is 16 bytes. The Xbox-ABI device proxy's CreateDescriptorHeap treats the two
// 8-byte words that follow a description as the console-extended slot-14 address/size (issue #313;
// docs/page_address_space.md), so a bare stack description is read through whatever happens to follow it - a saved
// pointer or an uninitialized slot that looks like a 64 KiB-aligned console VA takes the console-heap path. A PC
// caller supplies no such words, so pass this zero-tailed holder when the intent is a plain PC descriptor heap: the
// proxy reads address 0 and takes the arena path, whose GPU handle is the low arena VA the tests assert on.
#pragma once

#include <d3d12.h>

#include <cstddef>
#include <cstdint>

struct PcHeapDesc {
    D3D12_DESCRIPTOR_HEAP_DESC desc;
    std::uint64_t console_extension[2] = {};  // console slot-14 address, size: zero for a PC call
};

static_assert(sizeof(D3D12_DESCRIPTOR_HEAP_DESC) == 16,
              "the two console-extension words follow the 16-byte PC description");
static_assert(offsetof(PcHeapDesc, console_extension) == sizeof(D3D12_DESCRIPTOR_HEAP_DESC),
              "no padding between the PC description and the console-extension words");