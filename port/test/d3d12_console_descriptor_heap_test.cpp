// Console Xbox CreateDescriptorHeap (issue #313 comment 7107265058) passes a 16-byte PC
// D3D12_DESCRIPTOR_HEAP_DESC followed by address at +0x10 and size at +0x18. The title asserts
// GetCPUDescriptorHandleForHeapStart() == address. This tests the explicit Palmetto console-heap
// creation path used by the Xbox device proxy slot 14. It does not cover title-memory aliasing /
// section 3.3 backing.
#include "vk_test_device.h"
#include "../shims/d3d12_com.h"
#include "../shims/d3d12_xs_device.h"

#include <cstdint>
#include <cstdio>

namespace {

template <class T>
struct Ref {
    T* p = nullptr;
    ~Ref() { if (p) p->Release(); }
    T** put() {
        if (p) {
            p->Release();
            p = nullptr;
        }
        return &p;
    }
    T* get() const { return p; }
    T* operator->() const { return p; }
};

bool check_console_addressed_heap(ID3D12Device* device) {
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = 8;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    desc.NodeMask = 0;
    const D3D12_GPU_VIRTUAL_ADDRESS address = 0x112233440000ull;
    const std::uint64_t size = 8ull * 32ull;

    Ref<ID3D12DescriptorHeap> heap;
    const HRESULT hr = rt::d3d12::create_console_descriptor_heap(
        device, &desc, address, size, IID_PPV_ARGS(heap.put()));
    if (FAILED(hr) || !heap.get()) {
        std::fprintf(stderr, "create_console_descriptor_heap failed: 0x%08lx\n", static_cast<unsigned long>(hr));
        return false;
    }
    const auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
    if (cpu.ptr != static_cast<SIZE_T>(address)) {
        std::fprintf(stderr, "CPU heap start mismatch: got 0x%llx want 0x%llx\n",
                     static_cast<unsigned long long>(cpu.ptr), static_cast<unsigned long long>(address));
        return false;
    }
    const auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
    if (gpu.ptr != address) {
        std::fprintf(stderr, "GPU heap start mismatch: got 0x%llx want 0x%llx\n",
                     static_cast<unsigned long long>(gpu.ptr), static_cast<unsigned long long>(address));
        return false;
    }
    const D3D12_DESCRIPTOR_HEAP_DESC got = heap->GetDesc();
    if (got.Type != desc.Type || got.NumDescriptors != desc.NumDescriptors || got.Flags != desc.Flags ||
        got.NodeMask != 1) {
        std::fprintf(stderr, "GetDesc mismatch\n");
        return false;
    }
    return true;
}

bool check_xs_proxy_slot14(ID3D12Device* real_device) {
    // create_device_proxy takes ownership of the reference it is handed (d3d12_xs_device.h), so this caller keeps
    // its own: without the extra reference the device is released with the proxy and the caller's is a second one.
    real_device->AddRef();
    ID3D12Device* proxy_raw = nullptr;
    if (FAILED(rt::d3d12_xs::create_device_proxy(real_device, IID_PPV_ARGS(&proxy_raw))) || !proxy_raw) {
        std::fprintf(stderr, "create_device_proxy failed\n");
        return false;
    }
    Ref<ID3D12Device> proxy;
    proxy.p = proxy_raw;

    struct ConsoleDescriptorHeapDesc {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        std::uint64_t address = 0;
        std::uint64_t size = 0;
    } console{};
    console.desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    console.desc.NumDescriptors = 4;
    console.desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    console.address = 0x556677880000ull;
    console.size = 4ull * 32ull;

    Ref<ID3D12DescriptorHeap> heap;
    const HRESULT hr = proxy->CreateDescriptorHeap(&console.desc, IID_PPV_ARGS(heap.put()));
    if (FAILED(hr) || !heap.get()) {
        std::fprintf(stderr, "proxy CreateDescriptorHeap(console) failed: 0x%08lx\n",
                     static_cast<unsigned long>(hr));
        return false;
    }
    // The device proxy wraps the returned heap in an Xbox-ABI heap proxy. Its slots 9 and 10 are the console ABI:
    // the 8-byte handle comes back in RAX (d3d12_xs_device.h, "the heap thunk converts the PC hidden-pointer form"),
    // so the handle is read through a function pointer of that shape. A PC C++ virtual call would take it through
    // the hidden return pointer instead and see an unwritten value.
    using XboxHeapHandleFn = std::uint64_t(WINAPI*)(void* heap);
    void** vtable = *reinterpret_cast<void***>(heap.get());
    const auto cpu = reinterpret_cast<XboxHeapHandleFn>(vtable[9])(heap.get());
    if (cpu != console.address) {
        std::fprintf(stderr, "proxy CPU heap start mismatch: got 0x%llx want 0x%llx\n",
                     static_cast<unsigned long long>(cpu),
                     static_cast<unsigned long long>(console.address));
        return false;
    }
    const auto gpu = reinterpret_cast<XboxHeapHandleFn>(vtable[10])(heap.get());
    if (gpu != console.address) {
        std::fprintf(stderr, "proxy GPU heap start mismatch: got 0x%llx want 0x%llx\n",
                     static_cast<unsigned long long>(gpu),
                     static_cast<unsigned long long>(console.address));
        return false;
    }
    return true;
}

bool check_normal_heap_unchanged(ID3D12Device* device) {
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = 4;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Ref<ID3D12DescriptorHeap> heap;
    const HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(heap.put()));
    if (FAILED(hr) || !heap.get()) {
        std::fprintf(stderr, "CreateDescriptorHeap(normal) failed: 0x%08lx\n", static_cast<unsigned long>(hr));
        return false;
    }
    const auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
    if (cpu.ptr == 0) {
        std::fprintf(stderr, "normal heap CPU start is null\n");
        return false;
    }
    return true;
}

}  // namespace

int main() {
    vktest::Device vk;
    const auto status = vk.initialize();
    if (status == vktest::DeviceStatus::unavailable) return 77;
    if (status != vktest::DeviceStatus::ready) return 1;

    Ref<ID3D12Device> device;
    rt::d3d12::VulkanDeviceInfo info{};
    info.get_instance_proc = vk.instance_proc;
    info.instance = vk.instance;
    info.physical = vk.physical;
    info.device = vk.device;
    info.queue = vk.queue;
    info.queue_family = vk.family;
    if (FAILED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(device.put()))) || !device.get()) {
        std::fprintf(stderr, "create_vulkan_device failed\n");
        return 1;
    }

    if (!check_normal_heap_unchanged(device.get())) return 1;
    if (!check_console_addressed_heap(device.get())) return 1;
    if (!check_xs_proxy_slot14(device.get())) return 1;
    std::fprintf(stderr, "ok\n");
    return 0;
}
