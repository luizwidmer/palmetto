// Independent production-adapter Title-VA GPU acceptance (P-45).
// A synthetic raw VirtualAlloc span uses the production BDA allocation, resolver and page-pull path; a PC-ABI D3D12
// device/list reads two sampled pixels through its real queue/fence. No game data.
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
    if (!(condition)) { std::fprintf(stderr, "d3d12_title_va_production_test:%d: %s\n", __LINE__, #condition); return 1; } \
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

struct SyntheticTitleSpan {
    static constexpr std::uint64_t kBytes = 0x120000;
    void* address = VirtualAlloc(nullptr, static_cast<SIZE_T>(kBytes), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ~SyntheticTitleSpan() { if (address) VirtualFree(address, 0, MEM_RELEASE); }
    SyntheticTitleSpan() = default;
    SyntheticTitleSpan(const SyntheticTitleSpan&) = delete;
    SyntheticTitleSpan& operator=(const SyntheticTitleSpan&) = delete;
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


bool check_cpu_ordering_refusals() {
    constexpr std::uint64_t heap_base = 0x07400000;
    constexpr std::uint64_t arena_bytes = 0x10000000;  // 256 MiB, matching the production probe
    constexpr std::uint64_t arena_end = heap_base + arena_bytes;
    constexpr std::uint64_t raw_va = 0x0000015300000000;
    using Refusal = palmetto::TitleVaAliasRefusal;

    palmetto::TitleVaAliasTable below(palmetto::TitleVaWindowsSwitch::on, heap_base, arena_bytes);
    const auto below_result = below.add_window(1, heap_base - 0x1000, 64);
    if (below_result != Refusal::window_below_heap_base ||
        std::strcmp(palmetto::title_va_alias_refusal_name(below_result), "window_below_heap_base") != 0) {
        std::fprintf(stderr, "FAIL: expected named window_below_heap_base refusal, got %s\n",
                     palmetto::title_va_alias_refusal_name(below_result));
        return false;
    }

    palmetto::TitleVaAliasTable overlap(palmetto::TitleVaWindowsSwitch::on, heap_base, arena_bytes);
    const auto overlap_result = overlap.add_window(1, arena_end - 0x1000, 64);
    if (overlap_result != Refusal::window_overlaps_arena ||
        std::strcmp(palmetto::title_va_alias_refusal_name(overlap_result), "window_overlaps_arena") != 0) {
        std::fprintf(stderr, "FAIL: expected named window_overlaps_arena refusal, got %s\n",
                     palmetto::title_va_alias_refusal_name(overlap_result));
        return false;
    }

    palmetto::TitleVaAliasTable above(palmetto::TitleVaWindowsSwitch::on, heap_base, arena_bytes);
    if (above.add_window(1, arena_end, 64) != Refusal::none ||
        above.add_alias(1, raw_va, 64, 1) != Refusal::none) {
        std::fprintf(stderr, "FAIL: first outside-arena window did not register at arena_end\n");
        return false;
    }
    palmetto::TitleVaAliasResolution resolved{};
    if (above.resolve(raw_va, 64, resolved) != Refusal::none || resolved.device_address != arena_end ||
        resolved.console_va != arena_end - heap_base) {
        std::fprintf(stderr, "FAIL: accepted window did not preserve heap-relative BDA ordering\n");
        return false;
    }
    const auto oob = above.resolve(raw_va + 60, 8, resolved);
    if (oob != Refusal::alias_view_out_of_bounds ||
        std::strcmp(palmetto::title_va_alias_refusal_name(oob), "alias_view_out_of_bounds") != 0) {
        std::fprintf(stderr, "FAIL: expected named alias_view_out_of_bounds refusal, got %s\n",
                     palmetto::title_va_alias_refusal_name(oob));
        return false;
    }
    std::printf("Synthetic ordering policy: window_below_heap_base, window_overlaps_arena, and alias_view_out_of_bounds named; arena_end accepted\n");
    return true;
}

struct BdaCandidate {
    std::uint64_t candidate = 0;
    std::uint64_t heap_base = 0;
    std::uint64_t arena_begin = 0;
    std::uint64_t arena_end = 0;
    std::string ordering;
};

bool parse_bda_candidate(const std::string& log, BdaCandidate& out) {
    const auto marker = log.find("candidate BDA 0x");
    if (marker == std::string::npos) return false;
    unsigned long long candidate = 0, heap_base = 0, arena_begin = 0, arena_end = 0;
    char ordering[64]{};
    if (std::sscanf(log.c_str() + marker,
                    "candidate BDA 0x%llx; heap_base 0x%llx; arena [0x%llx,0x%llx); ordering=%63s",
                    &candidate, &heap_base, &arena_begin, &arena_end, ordering) != 5) return false;
    out.candidate = static_cast<std::uint64_t>(candidate);
    out.heap_base = static_cast<std::uint64_t>(heap_base);
    out.arena_begin = static_cast<std::uint64_t>(arena_begin);
    out.arena_end = static_cast<std::uint64_t>(arena_end);
    out.ordering = ordering;
    return true;
}

const char* production_unavailable_refusal(const std::string& log) {
    static const char* const names[] = {
        "window_below_heap_base", "window_overlaps_arena", "buffer_device_address_unavailable",
        "host_coherent_memory_unavailable", "window_buffer_create_failed", "window_memory_bind_failed",
        "window_memory_map_failed", "window_device_address_unavailable", "window_allocation_too_large",
        "window_v48_unrepresentable"};
    for (const char* name : names) if (log.find(name) != std::string::npos) return name;
    return nullptr;
}

bool record_draw_readback(ID3D12Device* device, ID3D12Resource* target, ID3D12Resource* readback,
                          ID3D12RootSignature* root, ID3D12PipelineState* pso,
                          D3D12_CPU_DESCRIPTOR_HANDLE rtv, ID3D12Resource* tint,
                          const D3D12_VERTEX_BUFFER_VIEW& vbv, UINT width, UINT height, UINT row_pitch,
                          D3D12_RESOURCE_STATES prior_target_state,
                          d3d12test::Ref<ID3D12CommandAllocator>& allocator,
                          d3d12test::Ref<ID3D12GraphicsCommandList>& list) {
    using namespace d3d12test;
    if (!make_list(device, allocator, list, pso)) return false;
    const auto to_rt = transition(target, prior_target_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ResourceBarrier(1, &to_rt);
    bind_draw_state(list.p, root, pso, rtv, tint, vbv, width, height);
    const auto to_copy = transition(target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &to_copy);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, row_pitch};
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = target;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    return SUCCEEDED(list->Close());
}

bool read_center_pixel(ID3D12Resource* readback, UINT height, UINT width, UINT row_pitch,
                       std::array<std::uint8_t, 4>& pixel) {
    void* mapped = nullptr;
    const D3D12_RANGE read_range{0, static_cast<SIZE_T>(row_pitch) * height};
    if (FAILED(readback->Map(0, &read_range, &mapped)) || !mapped) return false;
    const auto* at = static_cast<const std::uint8_t*>(mapped) + (height / 2) * row_pitch + (width / 2) * 4;
    pixel = {at[0], at[1], at[2], at[3]};
    const D3D12_RANGE no_cpu_writes{0, 0};
    readback->Unmap(0, &no_cpu_writes);
    return true;
}

bool log_has(const std::string& log, const char* marker, const char* name) {
    if (log.find(marker) != std::string::npos) return true;
    std::fprintf(stderr, "FAIL: production_%s_not_observed\n", name);
    return false;
}

}  // namespace

int main() {
    using namespace d3d12test;
    constexpr UINT width = 16;
    constexpr UINT height = 16;
    constexpr UINT row_pitch = 256;
    constexpr UINT readback_bytes = row_pitch * height;

    if (!check_cpu_ordering_refusals()) return 1;

    testfiles::ScratchDir scratch("palmetto_title_va_production_test");
    const auto& library = scratch.path();
    if (!prepare_shader_library(library)) {
        std::fprintf(stderr, "FAIL: synthetic_shader_library_unavailable\n");
        return 1;
    }

    ScopedEnvironment shader_env("PALMETTO_SHADERS");
    ScopedEnvironment windows_env("PALMETTO_TITLE_VA_WINDOWS");
    ScopedEnvironment title_va_env("PALMETTO_TITLE_VA");
    ScopedEnvironment page_pull_env("PALMETTO_PAGE_PULL");
    ScopedEnvironment arena_env("PALMETTO_ARENA_MB");
    ScopedEnvironment validation_env("PALMETTO_VK_VALIDATION");
    EXPECT(shader_env.set(library.string()));
    EXPECT(windows_env.set("1"));
    EXPECT(title_va_env.set("1"));
    EXPECT(page_pull_env.set("1"));
    EXPECT(arena_env.set("256"));
    EXPECT(validation_env.set("0"));  // functional pixel acceptance; validation-on P-48 diagnostic is tracked separately

    const auto vs_program = fssm_program({0x56530047, 0xbf810000});
    const auto ps_program = fssm_program({0x50530047, 0xbf810000});
    const auto vs_container = shader_container(vs_program);
    const auto ps_container = shader_container(ps_program);

    constexpr std::array<Vertex, 3> visible_triangle{{
        {-1.0f, -1.0f, {0x35, 0x47, 0xc1, 0xff}},
        { 3.0f, -1.0f, {0x35, 0x47, 0xc1, 0xff}},
        {-1.0f,  3.0f, {0x35, 0x47, 0xc1, 0xff}},
    }};
    constexpr std::array<Vertex, 3> offscreen_triangle{{
        {5.0f, 5.0f, {0x35, 0x47, 0xc1, 0xff}},
        {5.0f, 5.0f, {0x35, 0x47, 0xc1, 0xff}},
        {5.0f, 5.0f, {0x35, 0x47, 0xc1, 0xff}},
    }};
    SyntheticTitleSpan pages;
    if (!pages.address) {
        std::fprintf(stderr, "SKIP: synthetic_title_pages_unavailable: VirtualAlloc failed\n");
        return 77;
    }
    const auto raw_va = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pages.address));
    if (raw_va < palmetto::TitleVaAliasTable::kArenaWindowEnd) {
        std::fprintf(stderr, "SKIP: title_va_inside_arena_window: host VA is not outside 32-bit arena\n");
        return 77;
    }
    std::memset(pages.address, 0, static_cast<std::size_t>(SyntheticTitleSpan::kBytes));
    std::memcpy(pages.address, visible_triangle.data(), sizeof(visible_triangle));

    vktest::Device vk;
    vk.request_buffer_device_address = true;
    vk.request_debug_utils = true;
    const auto vk_status = vk.initialize();
    if (vk_status == vktest::DeviceStatus::unavailable) {
        std::fprintf(stderr, "SKIP: vulkan_gpu_or_required_features_unavailable (status %u)\n",
                     static_cast<unsigned>(vk_status));
        return 77;
    }
    if (vk_status != vktest::DeviceStatus::ready) {
        std::fprintf(stderr, "FAIL: vulkan_test_device_initialization_failed (status %u)\n",
                     static_cast<unsigned>(vk_status));
        return 1;
    }
    if (!vk.buffer_device_address) {
        std::fprintf(stderr, "SKIP: buffer_device_address_unavailable\n");
        return 77;
    }

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
    // Leave title_va_window_test_hooks empty: production VkBuffer/BDA allocation, resolver and raw-page pull are exercised.
    // Keep the PC-ABI D3D12 device direct; no Xbox queue wrapper may proxy Signal through console slot 14.
    Ref<ID3D12Device> device;
    const HRESULT create_hr = rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&device.p));
    if (FAILED(create_hr) || !device.p) {
        std::fprintf(stderr, "SKIP: d3d12_vulkan_device_unavailable (HRESULT 0x%08lx)\n",
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

    const D3D12_VERTEX_BUFFER_VIEW vbv{raw_va, static_cast<UINT>(SyntheticTitleSpan::kBytes),
                                      static_cast<UINT>(sizeof(Vertex))};
    Ref<ID3D12CommandAllocator> first_allocator;
    Ref<ID3D12GraphicsCommandList> first_list;
    EXPECT(record_draw_readback(device.p, target.p, readback.p, root.p, pso.p, rtv, tint_buffer.p, vbv,
                                width, height, row_pitch, D3D12_RESOURCE_STATE_COMMON, first_allocator, first_list));
    palmetto::ReplayResult first_result = palmetto::ReplayResult::success;
    std::string first_log;
    EXPECT(execute_and_wait(queue.p, fence.p, event.handle, first_list.p, 1, first_result, first_log));
    if (first_result != palmetto::ReplayResult::success) {
        if (const char* reason = production_unavailable_refusal(first_log)) {
            std::fprintf(stderr, "SKIP: production_window_unavailable: %s\n", reason);
            return 77;
        }
        std::fprintf(stderr, "FAIL: production_first_submit_refused (result %u)\n%s",
                     static_cast<unsigned>(first_result), first_log.c_str());
        return 1;
    }
    BdaCandidate bda{};
    if (!parse_bda_candidate(first_log, bda)) {
        std::fprintf(stderr, "FAIL: production_bda_candidate_diagnostic_missing_or_unparseable\n%s",
                     first_log.c_str());
        return 1;
    }
    if (bda.ordering != "at_or_above_arena_end" || bda.candidate < bda.arena_end ||
        bda.arena_begin != bda.heap_base || bda.candidate < bda.heap_base) {
        std::fprintf(stderr, "FAIL: production_bda_ordering_invalid (candidate=0x%llx heap_base=0x%llx arena=[0x%llx,0x%llx) ordering=%s)\n",
                     static_cast<unsigned long long>(bda.candidate), static_cast<unsigned long long>(bda.heap_base),
                     static_cast<unsigned long long>(bda.arena_begin), static_cast<unsigned long long>(bda.arena_end),
                     bda.ordering.c_str());
        return 1;
    }
    if (!log_has(first_log, "title VA window registered", "registration") ||
        !log_has(first_log, "outside-arena title VA window", "production_window") ||
        !log_has(first_log, "page pull: title VA", "raw_page_pull") ||
        first_log.find("queue slot 14") != std::string::npos) {
        if (first_log.find("queue slot 14") != std::string::npos)
            std::fprintf(stderr, "FAIL: xbox_queue_slot_14_proxy_observed\n");
        return 1;
    }
    std::array<std::uint8_t, 4> first_pixel{};
    EXPECT(read_center_pixel(readback.p, height, width, row_pitch, first_pixel));
    const std::array<std::uint8_t, 4> expected_draw{0x35, 0xff, 0xc1, 0xff};
    if (first_pixel != expected_draw) {
        std::fprintf(stderr, "FAIL: production_first_pixel got %02x %02x %02x %02x expected %02x %02x %02x %02x\n",
                     first_pixel[0], first_pixel[1], first_pixel[2], first_pixel[3], expected_draw[0],
                     expected_draw[1], expected_draw[2], expected_draw[3]);
        return 1;
    }

    // Mutate only synthetic raw title bytes; the second draw must pull them again before the GPU reads the same alias.
    std::memcpy(pages.address, offscreen_triangle.data(), sizeof(offscreen_triangle));
    Ref<ID3D12CommandAllocator> second_allocator;
    Ref<ID3D12GraphicsCommandList> second_list;
    EXPECT(record_draw_readback(device.p, target.p, readback.p, root.p, pso.p, rtv, tint_buffer.p, vbv,
                                width, height, row_pitch, D3D12_RESOURCE_STATE_COPY_SOURCE, second_allocator, second_list));
    palmetto::ReplayResult second_result = palmetto::ReplayResult::success;
    std::string second_log;
    EXPECT(execute_and_wait(queue.p, fence.p, event.handle, second_list.p, 2, second_result, second_log));
    EXPECT(second_result == palmetto::ReplayResult::success);
    if (!log_has(second_log, "page pull: title VA", "second_raw_page_pull")) return 1;
    if (second_log.find("queue slot 14") != std::string::npos) {
        std::fprintf(stderr, "FAIL: xbox_queue_slot_14_proxy_observed_on_second_fence\n");
        return 1;
    }
    std::array<std::uint8_t, 4> second_pixel{};
    EXPECT(read_center_pixel(readback.p, height, width, row_pitch, second_pixel));
    const std::array<std::uint8_t, 4> expected_clear{0x00, 0x00, 0x00, 0xff};
    if (second_pixel != expected_clear) {
        std::fprintf(stderr, "FAIL: refreshed_raw_page_pixel got %02x %02x %02x %02x expected clear %02x %02x %02x %02x\n",
                     second_pixel[0], second_pixel[1], second_pixel[2], second_pixel[3], expected_clear[0],
                     expected_clear[1], expected_clear[2], expected_clear[3]);
        return 1;
    }

    std::printf("Production Title-VA GPU PASS: raw span 0x%llx + 0x%llx; candidate BDA 0x%llx, heap_base 0x%llx, arena_end 0x%llx; registered, pulled in both submissions, pixels %02x %02x %02x %02x then %02x %02x %02x %02x through direct queue/fence\n",
                static_cast<unsigned long long>(raw_va),
                static_cast<unsigned long long>(SyntheticTitleSpan::kBytes),
                static_cast<unsigned long long>(bda.candidate), static_cast<unsigned long long>(bda.heap_base),
                static_cast<unsigned long long>(bda.arena_end), first_pixel[0], first_pixel[1], first_pixel[2],
                first_pixel[3], second_pixel[0], second_pixel[1], second_pixel[2], second_pixel[3]);
    return 0;
}
