// A bounded, independent GPU regression for strict multi-count viewport/scissor replay (P-47).
// It owns its Vulkan/D3D12 device, graphics list, PSO, target and readback; it does not use d3d12_draw_test's fixture.
// Requires the Khronos validation layer, and skips (77) when no usable GPU/capability set is available. No game data.
#include "d3d12_test_shaders.h"
#include "shader_fixture_path.h"
#include "test_temp_dir.h"
#include "../shims/d3d12_com.h"
#include "../src/shader_library.h"
#include "../src/viewport_replay_readiness.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#if !defined(PALMETTO_TEST_VS_FULLSCREEN_SPV) || !defined(PALMETTO_TEST_PS_SPV)
#error "PALMETTO_TEST_VS_FULLSCREEN_SPV and PALMETTO_TEST_PS_SPV must name the compiled test shaders"
#endif

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "d3d12_viewport_array_test:%d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

namespace {

class ScopedEnvironment {
public:
    explicit ScopedEnvironment(const char* name) : name_(name) {
        if (const char* old = std::getenv(name_.c_str())) {
            was_set_ = true;
            old_value_ = old;
        }
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

struct ValidationCounter {
    std::atomic_uint errors{0};
};

VKAPI_ATTR VkBool32 VKAPI_CALL on_validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT types,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) &&
        (types & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT)) {
        static_cast<ValidationCounter*>(user)->errors.fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr, "Vulkan validation error: %s\n", data && data->pMessage ? data->pMessage : "(no message)");
    }
    return VK_FALSE;
}

struct ValidationMessenger {
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT handle = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroy = nullptr;

    ~ValidationMessenger() { reset(); }
    ValidationMessenger(const ValidationMessenger&) = delete;
    ValidationMessenger& operator=(const ValidationMessenger&) = delete;
    ValidationMessenger() = default;

    void reset() {
        if (handle && destroy) destroy(instance, handle, nullptr);
        handle = VK_NULL_HANDLE;
    }
};

struct EventHandle {
    HANDLE handle = nullptr;
    ~EventHandle() { if (handle) CloseHandle(handle); }
    EventHandle() = default;
    EventHandle(const EventHandle&) = delete;
    EventHandle& operator=(const EventHandle&) = delete;
};

int run_viewport_draw(ID3D12Device* device) {
    using namespace d3d12test;
    constexpr UINT width = 32;
    constexpr UINT height = 32;
    constexpr UINT row_pitch = 256;
    constexpr UINT readback_bytes = row_pitch * height;

    // Synthetic shader IDs map the existing translator-contract test shaders into this private scratch library.
    const auto vs_program = fssm_program({0x56530047, 0xbf810000});
    const auto ps_program = fssm_program({0x50530047, 0xbf810000});
    const auto vs_container = shader_container(vs_program);
    const auto ps_container = shader_container(ps_program);

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
    pso_desc.InputLayout = {nullptr, 0};
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

    const auto upload_desc = buffer_desc(256);
    Ref<ID3D12Resource> tint_buffer;
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&tint_buffer.p))));
    void* tint_data = nullptr;
    const D3D12_RANGE no_cpu_reads{0, 0};
    EXPECT(SUCCEEDED(tint_buffer->Map(0, &no_cpu_reads, &tint_data)));
    const float green_tint[3] = {0.0f, 1.0f, 0.0f};
    std::memcpy(tint_data, green_tint, sizeof(green_tint));
    tint_buffer->Unmap(0, nullptr);

    const auto readback_desc = buffer_desc(readback_bytes);
    Ref<ID3D12Resource> readback;
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&readback.p))));

    const D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1,
                                              D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    Ref<ID3D12DescriptorHeap> rtv_heap;
    EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap.p))));
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.p, nullptr, rtv);

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Ref<ID3D12CommandQueue> queue;
    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> list;
    Ref<ID3D12Fence> fence;
    EXPECT(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
    EXPECT(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
    EXPECT(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, pso.p,
                                               IID_PPV_ARGS(&list.p))));
    EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));

    const auto to_target = transition(target.p, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ResourceBarrier(1, &to_target);
    const FLOAT clear[4] = {0, 0, 0, 1};
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    list->SetGraphicsRootSignature(root.p);
    list->SetPipelineState(pso.p);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const UINT32 zero_constants[2] = {0, 0};
    list->SetGraphicsRoot32BitConstants(0, 2, zero_constants, 0);
    list->SetGraphicsRootConstantBufferView(1, tint_buffer->GetGPUVirtualAddress());
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    // Two distinct entries exercise the strict dynamic-with-count PSO/commands; the synthetic shader uses the default
    // ViewportIndex (0), so (8,8) proves rasterization through element 0, while element 1 still travels in the count-2
    // state. The C-06 design note records that per-ViewportIndex routing is a separate, untested behavior.
    const D3D12_VIEWPORT viewports[2] = {
        {0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f},
        {float(width / 2), float(height / 2), float(width / 2), float(height / 2), 0.0f, 1.0f},
    };
    const D3D12_RECT scissors[2] = {
        {0, 0, LONG(width), LONG(height)},
        {LONG(width / 2), LONG(height / 2), LONG(width), LONG(height)},
    };
    list->RSSetViewports(2, viewports);
    list->RSSetScissorRects(2, scissors);
    list->DrawInstanced(3, 1, 0, 0);

    const auto to_copy = transition(target.p, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &to_copy);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback.p;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = {0, {DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, row_pitch}};
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = target.p;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    EXPECT(list->Close() == S_OK);

    ID3D12CommandList* lists[] = {list.p};
    queue->ExecuteCommandLists(1, lists);
    EXPECT(rt::d3d12::last_execute_result(queue.p) == palmetto::ReplayResult::success);
    EXPECT(SUCCEEDED(queue->Signal(fence.p, 1)));

    EventHandle event;
    event.handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    EXPECT(event.handle != nullptr);
    EXPECT(SUCCEEDED(fence->SetEventOnCompletion(1, event.handle)));
    EXPECT(WaitForSingleObject(event.handle, 30000) == WAIT_OBJECT_0);

    void* mapped = nullptr;
    const D3D12_RANGE read_range{0, readback_bytes};
    EXPECT(SUCCEEDED(readback->Map(0, &read_range, &mapped)));
    const auto* bytes = static_cast<const std::uint8_t*>(mapped);
    const auto* pixel = bytes + 8 * row_pitch + 8 * 4;
    const std::array<std::uint8_t, 4> expected{0, 255, 0, 255};
    const std::array<std::uint8_t, 4> actual{pixel[0], pixel[1], pixel[2], pixel[3]};
    readback->Unmap(0, nullptr);
    if (actual != expected) {
        std::fprintf(stderr, "pixel (8,8): got %u,%u,%u,%u, expected 0,255,0,255\n", actual[0], actual[1], actual[2],
                     actual[3]);
        return 1;
    }
    std::printf("pixel (8,8)=(0,255,0,255), two viewport/scissor entries\n");
    return 0;
}

}  // namespace

int main() {
    using namespace d3d12test;

    testfiles::ScratchDir scratch("palmetto_viewport_array_test");
    const auto& library = scratch.path();
    const auto vs_path = vktest::shader_fixture_path(PALMETTO_TEST_VS_FULLSCREEN_SPV);
    const auto ps_path = vktest::shader_fixture_path(PALMETTO_TEST_PS_SPV);
    EXPECT(testfiles::copy_file(vs_path, library / "vs_fullscreen.spv"));
    EXPECT(testfiles::copy_file(ps_path, library / "ps_draw.spv"));

    const auto vs_program = fssm_program({0x56530047, 0xbf810000});
    const auto ps_program = fssm_program({0x50530047, 0xbf810000});
    {
        std::ofstream index(library / "index.json", std::ios::binary);
        index << "{\"vs_fullscreen\": {\"stage\": \"vs\", \"fssm_sha256\": \""
              << palmetto::sha256_hex(vs_program)
              << "\", \"bindings\": [], \"push_constant_bytes\": 144, \"input_signature\": []},\n"
                 " \"ps_draw\": {\"stage\": \"ps\", \"fssm_sha256\": \""
              << palmetto::sha256_hex(ps_program)
              << "\", \"bindings\": [], \"push_constant_bytes\": 144, \"outputs\": [0]}}";
        EXPECT(bool(index));
    }

    ScopedEnvironment shader_env("PALMETTO_SHADERS");
    ScopedEnvironment strict_env(palmetto::viewport_replay_readiness::kStrictSwitch);
    ScopedEnvironment validation_env("PALMETTO_VK_VALIDATION");
    EXPECT(shader_env.set(library.string()));
    EXPECT(strict_env.set("1"));
    EXPECT(validation_env.set("1"));
    EXPECT(palmetto::viewport_replay_readiness::strict_enabled());

    Ref<ID3D12Device> device;
    const HRESULT create_hr = rt::d3d12::create_vulkan_device_standalone(IID_PPV_ARGS(&device.p));
    if (FAILED(create_hr) || !device.p) {
        std::fprintf(stderr, "SKIP: no Vulkan D3D12 device or validation layer (HRESULT 0x%08lx)\n",
                     static_cast<unsigned long>(create_hr));
        return 77;
    }
    const auto* info = rt::d3d12::vulkan_device_info(device.p);
    if (!info || !info->features.multiViewport || !info->dynamic_viewport_scissor_with_count ||
        info->max_viewport_count < 2) {
        std::fprintf(stderr,
                     "SKIP: multiViewport=%d, EDS-with-count=%d, maxViewports=%u; need all capabilities for count 2\n",
                     info ? int(info->features.multiViewport) : 0,
                     info ? int(info->dynamic_viewport_scissor_with_count) : 0,
                     info ? info->max_viewport_count : 0);
        return 77;
    }
    if (!info->get_instance_proc) {
        std::puts("SKIP: Vulkan instance proc loader is unavailable");
        return 77;
    }
    const auto get_device_proc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        info->get_instance_proc(info->instance, "vkGetDeviceProcAddr"));
    if (!get_device_proc ||
        !(get_device_proc(info->device, "vkCmdSetViewportWithCount") ||
          get_device_proc(info->device, "vkCmdSetViewportWithCountEXT")) ||
        !(get_device_proc(info->device, "vkCmdSetScissorWithCount") ||
          get_device_proc(info->device, "vkCmdSetScissorWithCountEXT"))) {
        std::puts("SKIP: viewport/scissor-with-count Vulkan commands are unavailable");
        return 77;
    }

    if (!info->debug_utils) {
        std::puts("SKIP: VK_EXT_debug_utils is unavailable for counting validation errors");
        return 77;
    }
    const auto create_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        info->get_instance_proc(info->instance, "vkCreateDebugUtilsMessengerEXT"));
    ValidationCounter validation;
    ValidationMessenger messenger;
    messenger.instance = info->instance;
    messenger.destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        info->get_instance_proc(info->instance, "vkDestroyDebugUtilsMessengerEXT"));
    if (!create_messenger || !messenger.destroy) {
        std::puts("SKIP: Vulkan validation messenger functions are unavailable");
        return 77;
    }
    VkDebugUtilsMessengerCreateInfoEXT messenger_info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    messenger_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    messenger_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
    messenger_info.pfnUserCallback = on_validation;
    messenger_info.pUserData = &validation;
    const VkResult messenger_result = create_messenger(info->instance, &messenger_info, nullptr, &messenger.handle);
    if (messenger_result != VK_SUCCESS) {
        std::fprintf(stderr, "SKIP: cannot create validation messenger (VkResult %d)\n", int(messenger_result));
        return 77;
    }

    EXPECT(run_viewport_draw(device.p) == 0);
    // The queue signal/wait in run_viewport_draw completed the GPU work before this assertion.
    const unsigned validation_errors = validation.errors.load(std::memory_order_relaxed);
    if (validation_errors) {
        std::fprintf(stderr, "FAIL: %u Vulkan validation error(s)\n", validation_errors);
        return 1;
    }
    std::printf("d3d12_viewport_array_test: PASS (0 validation errors, maxViewports=%u)\n", info->max_viewport_count);
    return 0;
}
