// Dedicated, opt-in Title-VA GPU regression (P-48).
// A caller-owned Vulkan test device supplies a coherent VkBuffer through the existing synthetic hook seam; a PC-ABI D3D12
// device/list draws from a raw VirtualAlloc VA and reads the pixel back through its real queue/fence. No game data.
#include "d3d12_test_shaders.h"
#include "shader_fixture_path.h"
#include "test_temp_dir.h"
#include "vk_test_device.h"
#include "../shims/d3d12_com.h"
#include "../src/shader_library.h"
#include "../src/title_va_alias.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <io.h>
#include <string>
#include <vector>

#if !defined(PALMETTO_TEST_VS_SPV) || !defined(PALMETTO_TEST_PS_SPV)
#error "PALMETTO_TEST_VS_SPV and PALMETTO_TEST_PS_SPV must name the compiled test shaders"
#endif

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "d3d12_title_va_test:%d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

namespace {

class ScopedEnvironment {
public:
    explicit ScopedEnvironment(const char* name) : name_(name) {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
        if (const char* old = std::getenv(name_.c_str())) {
            was_set_ = true;
            old_value_ = old;
        }
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    }
    ~ScopedEnvironment() { _putenv_s(name_.c_str(), was_set_ ? old_value_.c_str() : ""); }
    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;
    bool set(const std::string& value) const { return _putenv_s(name_.c_str(), value.c_str()) == 0; }
private:
    std::string name_;
    std::string old_value_;
    bool was_set_ = false;
};

class StderrCapture {
public:
    StderrCapture() = default;
    StderrCapture(const StderrCapture&) = delete;
    StderrCapture& operator=(const StderrCapture&) = delete;
    ~StderrCapture() { restore(); if (file_) std::fclose(file_); }

    bool begin() {
        file_ = std::tmpfile();
        if (!file_) return false;
        std::fflush(stderr);
        saved_fd_ = _dup(_fileno(stderr));
        if (saved_fd_ < 0) return false;
        if (_dup2(_fileno(file_), _fileno(stderr)) != 0) return false;
        active_ = true;
        return true;
    }

    std::string finish() {
        restore();
        if (!file_) return {};
        std::fflush(file_);
        if (std::fseek(file_, 0, SEEK_END) != 0) return {};
        const long length = std::ftell(file_);
        if (length < 0 || std::fseek(file_, 0, SEEK_SET) != 0) return {};
        std::string text(static_cast<std::size_t>(length), '\0');
        if (!text.empty()) std::fread(text.data(), 1, text.size(), file_);
        std::fclose(file_);
        file_ = nullptr;
        return text;
    }

private:
    void restore() {
        if (!active_) return;
        std::fflush(stderr);
        _dup2(saved_fd_, _fileno(stderr));
        _close(saved_fd_);
        saved_fd_ = -1;
        active_ = false;
    }
    FILE* file_ = nullptr;
    int saved_fd_ = -1;
    bool active_ = false;
};

struct SyntheticPage {
    void* address = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ~SyntheticPage() { if (address) VirtualFree(address, 0, MEM_RELEASE); }
    SyntheticPage() = default;
    SyntheticPage(const SyntheticPage&) = delete;
    SyntheticPage& operator=(const SyntheticPage&) = delete;
};

struct EventHandle {
    HANDLE handle = nullptr;
    ~EventHandle() { if (handle) CloseHandle(handle); }
    EventHandle() = default;
    EventHandle(const EventHandle&) = delete;
    EventHandle& operator=(const EventHandle&) = delete;
};

struct Vertex {
    float x;
    float y;
    std::uint8_t color[4];
};
static_assert(sizeof(Vertex) == 12);

// The test-only hook replaces only the outside-arena allocator/resolver/pull. Its backing buffer is created after the D3D12
// device has reserved its arena, and it remains owned by vktest::Device until the D3D12 device is released.
struct WindowHooks {
    vktest::Device* vk = nullptr;
    std::uint64_t expected_va = 0;
    std::uint64_t expected_bytes = 0;
    std::uint64_t registered_va = 0;
    std::uint64_t registered_bytes = 0;
    VkBuffer buffer = VK_NULL_HANDLE;
    void* mapped = nullptr;
    std::atomic<unsigned> allocate_calls{0};
    std::atomic<unsigned> resolve_calls{0};
    std::atomic<unsigned> resolve_hits{0};
    std::atomic<unsigned> pull_calls{0};
    std::atomic<unsigned> out_of_window_calls{0};
    std::atomic<unsigned> unexpected_calls{0};
    std::atomic_bool buffer_unavailable{false};
    std::atomic_bool bad_pull{false};

    bool contains(std::uint64_t va, std::uint64_t bytes) const {
        if (!registered_bytes || !bytes || va < registered_va) return false;
        const std::uint64_t offset = va - registered_va;
        return offset <= registered_bytes && bytes <= registered_bytes - offset;
    }

    bool allocate(std::uint64_t va, std::uint64_t bytes, VkBuffer& out_buffer, VkDeviceSize& out_offset,
                  const char*& why) {
        allocate_calls.fetch_add(1, std::memory_order_relaxed);
        if (buffer != VK_NULL_HANDLE) {
            if (va == registered_va && bytes == registered_bytes) {
                out_buffer = buffer;
                out_offset = 0;
                why = nullptr;
                return true;
            }
            if (va >= registered_va && va - registered_va <= registered_bytes &&
                bytes > registered_bytes - (va - registered_va)) {
                out_of_window_calls.fetch_add(1, std::memory_order_relaxed);
                why = "alias_view_out_of_bounds";
                return false;
            }
            unexpected_calls.fetch_add(1, std::memory_order_relaxed);
            why = "title_va_test_unexpected_window";
            return false;
        }
        if (!vk || va != expected_va || bytes != expected_bytes) {
            unexpected_calls.fetch_add(1, std::memory_order_relaxed);
            why = "title_va_test_unexpected_window";
            return false;
        }
        void* host = nullptr;
        const VkBufferUsageFlags usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                         VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        const VkBuffer made = vk->buffer(bytes, usage, &host);
        if (made == VK_NULL_HANDLE || !host) {
            buffer_unavailable.store(true, std::memory_order_relaxed);
            why = "test_hook_host_coherent_buffer_unavailable";
            return false;
        }
        buffer = made;
        mapped = host;
        registered_va = va;
        registered_bytes = bytes;
        out_buffer = buffer;
        out_offset = 0;
        why = nullptr;
        return true;
    }

    bool resolve(std::uint64_t va, std::uint64_t bytes, VkBuffer& out_buffer, VkDeviceSize& out_offset) {
        resolve_calls.fetch_add(1, std::memory_order_relaxed);
        if (buffer == VK_NULL_HANDLE || !mapped || !contains(va, bytes)) return false;
        out_buffer = buffer;
        out_offset = static_cast<VkDeviceSize>(va - registered_va);
        resolve_hits.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void pull(std::uint64_t va, std::uint64_t bytes) {
        if (!mapped || !contains(va, bytes)) {
            bad_pull.store(true, std::memory_order_relaxed);
            return;
        }
        auto* destination = static_cast<std::uint8_t*>(mapped) + (va - registered_va);
        const auto* source = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(va));
        std::memcpy(destination, source, static_cast<std::size_t>(bytes));
        pull_calls.fetch_add(1, std::memory_order_relaxed);
    }
};

bool check_cpu_alias_refusal() {
    constexpr std::uint64_t heap_base = 0x07400000;
    constexpr std::uint64_t arena_bytes = 0x00100000;
    constexpr std::uint64_t raw_va = 0x0000000153400000;
    palmetto::TitleVaAliasTable aliases(palmetto::TitleVaWindowsSwitch::on, heap_base, arena_bytes);
    if (aliases.add_window(1, heap_base + arena_bytes, 64) != palmetto::TitleVaAliasRefusal::none) return false;
    if (aliases.add_alias(1, raw_va, 32, 1) != palmetto::TitleVaAliasRefusal::none) return false;
    palmetto::TitleVaAliasResolution valid{};
    if (aliases.resolve(raw_va, 32, valid) != palmetto::TitleVaAliasRefusal::none) return false;
    palmetto::TitleVaAliasResolution refused{};
    const auto refusal = aliases.resolve(raw_va + 28, 8, refused);
    return refusal == palmetto::TitleVaAliasRefusal::alias_view_out_of_bounds &&
           std::strcmp(palmetto::title_va_alias_refusal_name(refusal), "alias_view_out_of_bounds") == 0;
}

bool prepare_shader_library(const std::filesystem::path& library) {
    const auto vs_path = vktest::shader_fixture_path(PALMETTO_TEST_VS_SPV);
    const auto ps_path = vktest::shader_fixture_path(PALMETTO_TEST_PS_SPV);
    if (!testfiles::copy_file(vs_path, library / "vs_draw.spv") ||
        !testfiles::copy_file(ps_path, library / "ps_draw.spv")) return false;
    const auto vs_program = d3d12test::fssm_program({0x56530047, 0xbf810000});
    const auto ps_program = d3d12test::fssm_program({0x50530047, 0xbf810000});
    std::ofstream index(library / "index.json", std::ios::binary);
    if (!index) return false;
    index << "{\"vs_draw\":{\"stage\":\"vs\",\"fssm_sha256\":\""
          << palmetto::sha256_hex(vs_program)
          << "\",\"bindings\":[],\"push_constant_bytes\":144,\"input_signature\":["
             "{\"semantic\":\"COLOR\",\"semantic_index\":0,\"register\":0,\"mask\":15,\"rw_mask\":15,"
             "\"system_value\":0,\"component_type\":\"float\"},"
             "{\"semantic\":\"POSITION\",\"semantic_index\":0,\"register\":1,\"mask\":3,\"rw_mask\":3,"
             "\"system_value\":0,\"component_type\":\"float\"}]},"
             "\"ps_draw\":{\"stage\":\"ps\",\"fssm_sha256\":\""
          << palmetto::sha256_hex(ps_program)
          << "\",\"bindings\":[],\"push_constant_bytes\":144,\"outputs\":[0]}}";
    return bool(index);
}

bool make_list(ID3D12Device* device, d3d12test::Ref<ID3D12CommandAllocator>& allocator,
               d3d12test::Ref<ID3D12GraphicsCommandList>& list, ID3D12PipelineState* pso) {
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p)))) return false;
    return SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, pso,
                                               IID_PPV_ARGS(&list.p)));
}

bool execute_and_wait(ID3D12CommandQueue* queue, ID3D12Fence* fence, HANDLE event,
                      ID3D12GraphicsCommandList* list, UINT64 fence_value,
                      palmetto::ReplayResult& result, std::string& log) {
    StderrCapture capture;
    if (!capture.begin()) return false;
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    result = rt::d3d12::last_execute_result(queue);
    if (FAILED(queue->Signal(fence, fence_value))) return false;
    if (FAILED(fence->SetEventOnCompletion(fence_value, event))) return false;
    if (WaitForSingleObject(event, 30000) != WAIT_OBJECT_0) return false;
    log = capture.finish();
    return true;
}

void bind_draw_state(ID3D12GraphicsCommandList* list, ID3D12RootSignature* root, ID3D12PipelineState* pso,
                     D3D12_CPU_DESCRIPTOR_HANDLE rtv, ID3D12Resource* tint, const D3D12_VERTEX_BUFFER_VIEW& vbv,
                     UINT width, UINT height) {
    const FLOAT clear[4] = {0, 0, 0, 1};
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    list->SetGraphicsRootSignature(root);
    list->SetPipelineState(pso);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const UINT32 zero_offset[2] = {0, 0};
    list->SetGraphicsRoot32BitConstants(0, 2, zero_offset, 0);
    list->SetGraphicsRootConstantBufferView(1, tint->GetGPUVirtualAddress());
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, LONG(width), LONG(height)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetVertexBuffers(0, 1, &vbv);
    list->DrawInstanced(3, 1, 0, 0);
}

}  // namespace

int main() {
    using namespace d3d12test;
    constexpr UINT width = 16;
    constexpr UINT height = 16;
    constexpr UINT row_pitch = 256;
    constexpr UINT readback_bytes = row_pitch * height;

    if (!check_cpu_alias_refusal()) {
        std::fprintf(stderr, "TitleVaAliasTable did not name the out-of-window view refusal\n");
        return 1;
    }

    testfiles::ScratchDir scratch("palmetto_title_va_test");
    const auto& library = scratch.path();
    if (!prepare_shader_library(library)) {
        std::fprintf(stderr, "could not prepare synthetic shader library\n");
        return 1;
    }

    ScopedEnvironment shader_env("PALMETTO_SHADERS");
    ScopedEnvironment windows_env("PALMETTO_TITLE_VA_WINDOWS");
    ScopedEnvironment title_va_env("PALMETTO_TITLE_VA");
    ScopedEnvironment page_pull_env("PALMETTO_PAGE_PULL");
    ScopedEnvironment arena_env("PALMETTO_ARENA_MB");
    EXPECT(shader_env.set(library.string()));
    EXPECT(windows_env.set("1"));
    EXPECT(title_va_env.set("1"));
    EXPECT(page_pull_env.set("1"));
    EXPECT(arena_env.set("256"));
    // The registered CTest sets PALMETTO_VK_VALIDATION=1; leave that setting intact.

    const auto vs_program = fssm_program({0x56530047, 0xbf810000});
    const auto ps_program = fssm_program({0x50530047, 0xbf810000});
    const auto vs_container = shader_container(vs_program);
    const auto ps_container = shader_container(ps_program);

    constexpr std::array<Vertex, 3> vertices{{
        {-1.0f, -1.0f, {0x35, 0x47, 0xc1, 0xff}},
        { 3.0f, -1.0f, {0x35, 0x47, 0xc1, 0xff}},
        {-1.0f,  3.0f, {0x35, 0x47, 0xc1, 0xff}},
    }};
    SyntheticPage page;
    if (!page.address) {
        std::fprintf(stderr, "SKIP: could not reserve synthetic host page\n");
        return 77;
    }
    const auto raw_va = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(page.address));
    if (raw_va < palmetto::TitleVaAliasTable::kArenaWindowEnd) {
        std::fprintf(stderr, "SKIP: host allocator did not provide an outside-arena VA\n");
        return 77;
    }
    std::memcpy(page.address, vertices.data(), sizeof(vertices));

    vktest::Device vk;
    vk.request_buffer_device_address = true;
    vk.request_debug_utils = true;
    const auto vk_status = vk.initialize();
    if (vk_status != vktest::DeviceStatus::ready) {
        std::fprintf(stderr, "SKIP: Vulkan GPU/BDA/validation test device unavailable (status %u)\n",
                     static_cast<unsigned>(vk_status));
        return 77;
    }
    if (!vk.buffer_device_address) {
        std::fprintf(stderr, "SKIP: Vulkan bufferDeviceAddress is unavailable\n");
        return 77;
    }

    WindowHooks hook{};
    hook.vk = &vk;
    hook.expected_va = raw_va;
    hook.expected_bytes = sizeof(vertices);
    rt::d3d12::VulkanDeviceInfo info{};
    info.get_instance_proc = vk.instance_proc;
    info.instance = vk.instance;
    info.physical = vk.physical;
    info.device = vk.device;
    info.queue = vk.queue;
    info.queue_family = vk.family;
    info.buffer_device_address = vk.buffer_device_address;
    info.debug_utils = vk.debug_utils;
    info.features = vk.enabled;
    info.subgroup = vk.subgroup;
    info.shader_dir = library.string();
    info.title_va_window_test_hooks.allocate = [&hook](std::uint64_t va, std::uint64_t bytes, VkBuffer& buffer,
                                                       VkDeviceSize& offset, const char*& why) {
        return hook.allocate(va, bytes, buffer, offset, why);
    };
    info.title_va_window_test_hooks.resolve = [&hook](std::uint64_t va, std::uint64_t bytes, VkBuffer& buffer,
                                                      VkDeviceSize& offset) {
        return hook.resolve(va, bytes, buffer, offset);
    };
    info.title_va_window_test_hooks.pull = [&hook](std::uint64_t va, std::uint64_t bytes) { hook.pull(va, bytes); };

    // create_vulkan_device returns the PC-ABI D3D12 implementation directly. Do not wrap it in the Xbox queue proxy:
    // the real ID3D12CommandQueue::Signal/ID3D12Fence path below is the synchronization under test.
    Ref<ID3D12Device> device;
    const HRESULT create_hr = rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&device.p));
    if (FAILED(create_hr) || !device.p) {
        std::fprintf(stderr, "SKIP: no usable D3D12/Vulkan device for Title-VA hooks (HRESULT 0x%08lx)\n",
                     static_cast<unsigned long>(create_hr));
        return 77;
    }

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, 2};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor = {1, 0};
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    root_desc.Desc_1_0 = {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    const auto root_blob = serialize_root_signature(root_desc);
    EXPECT(!root_blob.empty());
    Ref<ID3D12RootSignature> root;
    EXPECT(SUCCEEDED(device->CreateRootSignature(0, root_blob.data(), root_blob.size(), IID_PPV_ARGS(&root.p))));

    const D3D12_INPUT_ELEMENT_DESC elements[2] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D12_APPEND_ALIGNED_ELEMENT,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = root.p;
    pso_desc.VS = {vs_container.data(), vs_container.size()};
    pso_desc.PS = {ps_container.data(), ps_container.size()};
    pso_desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    pso_desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    pso_desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    pso_desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    pso_desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    pso_desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    pso_desc.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso_desc.SampleMask = 0xffffffff;
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    pso_desc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    pso_desc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;
    pso_desc.InputLayout = {elements, 2};
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso_desc.SampleDesc.Count = 1;
    Ref<ID3D12PipelineState> pso;
    EXPECT(SUCCEEDED(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso.p))));

    const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
    const D3D12_HEAP_PROPERTIES upload_heap{D3D12_HEAP_TYPE_UPLOAD};
    const D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC target_desc{};
    target_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    target_desc.Width = width;
    target_desc.Height = height;
    target_desc.DepthOrArraySize = 1;
    target_desc.MipLevels = 1;
    target_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    target_desc.SampleDesc.Count = 1;
    target_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    target_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    Ref<ID3D12Resource> target;
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &target_desc,
                                                     D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&target.p))));
    const auto tint_desc = buffer_desc(256);
    Ref<ID3D12Resource> tint_buffer;
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &tint_desc,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&tint_buffer.p))));
    void* tint_data = nullptr;
    const D3D12_RANGE no_cpu_reads{0, 0};
    EXPECT(SUCCEEDED(tint_buffer->Map(0, &no_cpu_reads, &tint_data)));
    const float tint[3] = {1.0f, 1.0f, 1.0f};
    std::memcpy(tint_data, tint, sizeof(tint));

    Ref<ID3D12Resource> readback;
    const auto readback_desc = buffer_desc(readback_bytes);
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&readback.p))));
    const D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    Ref<ID3D12DescriptorHeap> rtv_heap;
    EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap.p))));
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.p, nullptr, rtv);

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Ref<ID3D12CommandQueue> queue;
    Ref<ID3D12Fence> fence;
    EXPECT(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
    EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));
    EventHandle event;
    event.handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    EXPECT(event.handle != nullptr);

    const D3D12_VERTEX_BUFFER_VIEW vbv{raw_va, static_cast<UINT>(sizeof(vertices)), sizeof(Vertex)};
    Ref<ID3D12CommandAllocator> draw_allocator;
    Ref<ID3D12GraphicsCommandList> draw_list;
    EXPECT(make_list(device.p, draw_allocator, draw_list, pso.p));
    const auto target_to_rt = transition(target.p, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
    draw_list->ResourceBarrier(1, &target_to_rt);
    bind_draw_state(draw_list.p, root.p, pso.p, rtv, tint_buffer.p, vbv, width, height);
    const auto target_to_copy = transition(target.p, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    draw_list->ResourceBarrier(1, &target_to_copy);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback.p;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, row_pitch};
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = target.p;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    draw_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    EXPECT(SUCCEEDED(draw_list->Close()));
    palmetto::ReplayResult draw_result = palmetto::ReplayResult::success;
    std::string draw_log;
    EXPECT(execute_and_wait(queue.p, fence.p, event.handle, draw_list.p, 1, draw_result, draw_log));
    if (hook.buffer_unavailable.load(std::memory_order_relaxed)) {
        std::fprintf(stderr, "SKIP: host-coherent Vulkan test hook buffer unavailable\n");
        return 77;
    }
    EXPECT(draw_result == palmetto::ReplayResult::success);
    EXPECT(draw_log.find("title VA window registered") != std::string::npos);
    EXPECT(hook.buffer != VK_NULL_HANDLE);
    EXPECT(hook.allocate_calls.load(std::memory_order_relaxed) == 1);
    EXPECT(hook.resolve_hits.load(std::memory_order_relaxed) > 0);
    EXPECT(hook.pull_calls.load(std::memory_order_relaxed) > 0);
    EXPECT(!hook.bad_pull.load(std::memory_order_relaxed));
    EXPECT(hook.unexpected_calls.load(std::memory_order_relaxed) == 0);

    void* mapped_readback = nullptr;
    const D3D12_RANGE read_range{0, readback_bytes};
    EXPECT(SUCCEEDED(readback->Map(0, &read_range, &mapped_readback)));
    const auto* pixel = static_cast<const std::uint8_t*>(mapped_readback) + (height / 2) * row_pitch + (width / 2) * 4;
    const std::array<std::uint8_t, 4> actual{pixel[0], pixel[1], pixel[2], pixel[3]};
    const std::array<std::uint8_t, 4> expected{0x35, 0xff, 0xc1, 0xff};
    readback->Unmap(0, &no_cpu_reads);
    if (actual != expected) {
        std::fprintf(stderr, "Title-VA sampled pixel got %u,%u,%u,%u expected %u,%u,%u,%u\n",
                     actual[0], actual[1], actual[2], actual[3], expected[0], expected[1], expected[2], expected[3]);
        return 1;
    }
    EXPECT(std::memcmp(hook.mapped, page.address, sizeof(vertices)) == 0);

    // A view that starts in the registered raw range but extends past its end is refused by name and never resolves partially.
    Ref<ID3D12CommandAllocator> oob_allocator;
    Ref<ID3D12GraphicsCommandList> oob_list;
    EXPECT(make_list(device.p, oob_allocator, oob_list, pso.p));
    const auto target_to_rt_again = transition(target.p, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    oob_list->ResourceBarrier(1, &target_to_rt_again);
    const D3D12_VERTEX_BUFFER_VIEW oob_vbv{raw_va + sizeof(vertices) - 8, 16, sizeof(Vertex)};
    bind_draw_state(oob_list.p, root.p, pso.p, rtv, tint_buffer.p, oob_vbv, width, height);
    EXPECT(SUCCEEDED(oob_list->Close()));
    palmetto::ReplayResult oob_result = palmetto::ReplayResult::success;
    std::string oob_log;
    EXPECT(execute_and_wait(queue.p, fence.p, event.handle, oob_list.p, 2, oob_result, oob_log));
    EXPECT(oob_result == palmetto::ReplayResult::unresolved_resource);
    EXPECT(oob_log.find("alias_view_out_of_bounds") != std::string::npos);
    EXPECT(hook.out_of_window_calls.load(std::memory_order_relaxed) == 1);

    // WriteBufferImmediate is recorded through the real PC ID3D12GraphicsCommandList2 and refused before it can mutate the raw VA.
    Ref<ID3D12CommandAllocator> write_allocator;
    Ref<ID3D12GraphicsCommandList> write_list;
    EXPECT(make_list(device.p, write_allocator, write_list, pso.p));
    Ref<ID3D12GraphicsCommandList2> write_list2;
    EXPECT(SUCCEEDED(write_list->QueryInterface(IID_PPV_ARGS(&write_list2.p))));
    const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER immediate{raw_va, 0x12345678};
    write_list2->WriteBufferImmediate(1, &immediate, nullptr);
    EXPECT(SUCCEEDED(write_list->Close()));
    palmetto::ReplayResult write_result = palmetto::ReplayResult::success;
    std::string write_log;
    EXPECT(execute_and_wait(queue.p, fence.p, event.handle, write_list.p, 3, write_result, write_log));
    EXPECT(write_result == palmetto::ReplayResult::unresolved_resource);
    EXPECT(write_log.find("write_buffer_immediate_unsupported") != std::string::npos);
    EXPECT(std::memcmp(page.address, vertices.data(), sizeof(vertices)) == 0);
    EXPECT(vk.validation_errors() == 0);

    std::printf("Title-VA GPU test: raw VA registered/resolved, %u pull(s), sampled pixel %02x %02x %02x %02x; named OOB/WBI refusals\n",
                hook.pull_calls.load(std::memory_order_relaxed), actual[0], actual[1], actual[2], actual[3]);
    return 0;
}
