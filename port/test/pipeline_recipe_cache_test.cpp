// The per-process pipeline recipe cache (PALMETTO_PIPELINE_RECIPE_CACHE, docs/pipeline_cache.md): identical and
// differing descriptions are created and released in several orders, so a shared entry's reference count is exercised
// on both the adopt and the release side. Runs on the Vulkan-backed D3D12 device with synthetic GLSL modules; no game
// data, no window and no draws. Set PALMETTO_VK_VALIDATION=1 to check the lifetime under VK_LAYER_KHRONOS_validation:
// a premature destroy of a shared pipeline, module or layout, or a use of a released one, is reported there.
//
// The test turns the switch on itself (before the device is created) so it always exercises the cache; the switch's
// default-off behaviour is the rest of the suite's.
#include "d3d12_test_shaders.h"
#include "shader_fixture_path.h"
#include "test_temp_dir.h"
#include "vk_test_device.h"
#include "../shims/d3d12_com.h"
#include "../src/shader_library.h"

#include <d3d12.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#if !defined(PALMETTO_TEST_VS_FULLSCREEN_SPV) || !defined(PALMETTO_TEST_PS_SPV) || !defined(PALMETTO_TEST_CS_SPV)
#error "Test shader SPIR-V paths must be defined"
#endif

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

using namespace d3d12test;

namespace {

struct DeviceRef {
    ID3D12Device* device = nullptr;
    ~DeviceRef() {
        if (device) device->Release();
    }
};

struct PsoRef {
    ID3D12PipelineState* pso = nullptr;
    PsoRef() = default;
    explicit PsoRef(ID3D12PipelineState* p) : pso(p) {}
    PsoRef(const PsoRef&) = delete;
    PsoRef& operator=(const PsoRef&) = delete;
    ~PsoRef() { release(); }
    void release() {
        if (pso) {
            pso->Release();
            pso = nullptr;
        }
    }
    PsoRef(PsoRef&& other) noexcept : pso(other.pso) { other.pso = nullptr; }
    PsoRef& operator=(PsoRef&& other) noexcept {
        if (this != &other) {
            release();
            pso = other.pso;
            other.pso = nullptr;
        }
        return *this;
    }
};

}  // namespace

int main() {
    // The switch is read once, on the first pipeline creation; set it before any device exists.
    SetEnvironmentVariableA("PALMETTO_PIPELINE_RECIPE_CACHE", "1");

    vktest::Device vk_dev;
    const auto status = vk_dev.initialize();
    if (status == vktest::DeviceStatus::unavailable) {
        std::fprintf(stderr, "SKIP: no usable Vulkan 1.3 device\n");
        return 77;
    }
    EXPECT(status == vktest::DeviceStatus::ready);

    // A synthetic translated-shader library for the layer: the same shape d3d12_pso_validation_test builds.
    const auto vs_prog = fssm_program({0x56531001, 0xbf810000});
    const auto ps_prog = fssm_program({0x50531001, 0xbf810000});
    const auto cs_prog = fssm_program({0x43531001, 0xbf810000});
    const auto vs_container = shader_container(vs_prog);
    const auto ps_container = shader_container(ps_prog);
    const auto cs_container = shader_container(cs_prog);

    const testfiles::ScratchDir scratch("palmetto_pipeline_recipe_cache_test");
    const auto& lib_dir = scratch.path();
    EXPECT(testfiles::copy_file(vktest::shader_fixture_path(PALMETTO_TEST_VS_FULLSCREEN_SPV),
                                lib_dir / "vs_fullscreen.spv"));
    EXPECT(testfiles::copy_file(vktest::shader_fixture_path(PALMETTO_TEST_PS_SPV), lib_dir / "ps_draw.spv"));
    EXPECT(testfiles::copy_file(vktest::shader_fixture_path(PALMETTO_TEST_CS_SPV), lib_dir / "cs_user_data.spv"));
    {
        std::ofstream index(lib_dir / "index.json", std::ios::binary);
        index << "{\"vs_fullscreen\": {\"stage\": \"vs\", \"fssm_sha256\": \"" << palmetto::sha256_hex(vs_prog)
              << "\", \"bindings\": [], \"push_constant_bytes\": 144, \"input_signature\": []},\n"
                 " \"ps_draw\": {\"stage\": \"ps\", \"fssm_sha256\": \""
              << palmetto::sha256_hex(ps_prog)
              << "\", \"bindings\": [], \"push_constant_bytes\": 144, \"outputs\": [0]},\n"
                 " \"cs_user_data\": {\"stage\": \"cs\", \"fssm_sha256\": \""
              << palmetto::sha256_hex(cs_prog)
              << "\", \"bindings\": [], \"push_constant_bytes\": 144}}";
    }

    rt::d3d12::VulkanDeviceInfo info{};
    info.get_instance_proc = vk_dev.instance_proc;
    info.instance = vk_dev.instance;
    info.physical = vk_dev.physical;
    info.device = vk_dev.device;
    info.queue = vk_dev.queue;
    info.queue_family = vk_dev.family;
    info.buffer_device_address = true;
    info.subgroup = vk_dev.subgroup;
    info.shader_dir = lib_dir.string();
    info.features.logicOp = vk_dev.enabled.logicOp;
    info.vertex_attribute_divisor = vk_dev.vertex_attribute_divisor;

    DeviceRef layer;
    EXPECT(SUCCEEDED(rt::d3d12::create_vulkan_device(info, __uuidof(ID3D12Device), reinterpret_cast<void**>(&layer.device))));

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    root_desc.Desc_1_0.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    const auto root_blob = serialize_root_signature(root_desc);
    EXPECT(!root_blob.empty());
    ID3D12RootSignature* root = nullptr;
    EXPECT(SUCCEEDED(layer.device->CreateRootSignature(0, root_blob.data(), root_blob.size(),
                                                       __uuidof(ID3D12RootSignature),
                                                       reinterpret_cast<void**>(&root))));

    const D3D12_SHADER_BYTECODE vs{vs_container.data(), vs_container.size()};
    const D3D12_SHADER_BYTECODE ps{ps_container.data(), ps_container.size()};
    const D3D12_SHADER_BYTECODE cs{cs_container.data(), cs_container.size()};

    auto graphics_desc = [&](DXGI_FORMAT format, bool blend) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = root;
        desc.VS = vs;
        desc.PS = ps;
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.BlendState.RenderTarget[0].BlendEnable = blend ? TRUE : FALSE;
        if (blend) {
            desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
            desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
            desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
            desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
            desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
            desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        }
        desc.SampleMask = 0xffffffffu;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = format;
        desc.SampleDesc.Count = 1;
        return desc;
    };
    auto create = [&](const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc) -> PsoRef {
        ID3D12PipelineState* pso = nullptr;
        if (FAILED(layer.device->CreateGraphicsPipelineState(&desc, __uuidof(ID3D12PipelineState),
                                                             reinterpret_cast<void**>(&pso))))
            return PsoRef();
        return PsoRef(pso);
    };

    const D3D12_GRAPHICS_PIPELINE_STATE_DESC plain = graphics_desc(DXGI_FORMAT_R8G8B8A8_UNORM, false);
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC blended = graphics_desc(DXGI_FORMAT_R8G8B8A8_UNORM, true);
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC uint_target = graphics_desc(DXGI_FORMAT_R8G8B8A8_UINT, false);

    // 1. One description asked for many times: every call must answer S_OK, whether it creates or adopts.
    {
        std::vector<PsoRef> state;
        for (int i = 0; i < 24; ++i) {
            state.push_back(create(plain));
            EXPECT(state.back().pso != nullptr);
        }
        // Release every fourth one while the rest stay alive, then ask for it again: a new entry must be created and
        // the live adopters must be unaffected.
        for (int i = 0; i < 24; i += 4) state[static_cast<std::size_t>(i)].release();
        for (int i = 0; i < 8; ++i) {
            state.push_back(create(plain));
            EXPECT(state.back().pso != nullptr);
        }
        // Reverse order release: the last reference to the shared entry is not the last PSO created.
        while (!state.empty()) state.pop_back();
    }

    // 2. Different descriptions must not be confused: each format/blend state is its own entry, and releasing one
    // description's PSOs must not disturb the others.
    {
        PsoRef a = create(plain);
        PsoRef b = create(blended);
        PsoRef c = create(uint_target);
        EXPECT(a.pso && b.pso && c.pso);
        PsoRef a2 = create(plain);
        EXPECT(a2.pso != nullptr);
        a.release();  // the description stays shared through a2
        PsoRef b2 = create(blended);
        EXPECT(b2.pso != nullptr);
        c.release();
        b.release();
        PsoRef c2 = create(uint_target);
        EXPECT(c2.pso != nullptr);
    }

    // 3. Interleaved descriptions, many rounds, released in creation order: the entries are adopted and released
    // across each other's lifetimes.
    {
        std::vector<PsoRef> round;
        for (int i = 0; i < 16; ++i) {
            round.push_back(create(plain));
            round.push_back(create(blended));
        }
        for (auto& pso : round) EXPECT(pso.pso != nullptr);
    }

    // 4. After every PSO is gone the entries are gone too; a fresh identical request must create again and succeed.
    {
        PsoRef fresh = create(plain);
        EXPECT(fresh.pso != nullptr);
    }

    // 5. Compute: identical descriptions are adopted, a different one is not.
    auto create_compute = [&](std::uint32_t local_x) -> PsoRef {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = root;
        desc.CS = cs;
        (void)local_x;  // the synthetic test program's local size is fixed; the root signature is what varies the key
        ID3D12PipelineState* pso = nullptr;
        if (FAILED(layer.device->CreateComputePipelineState(&desc, __uuidof(ID3D12PipelineState),
                                                            reinterpret_cast<void**>(&pso))))
            return PsoRef();
        return PsoRef(pso);
    };
    {
        std::vector<PsoRef> compute;
        for (int i = 0; i < 8; ++i) {
            compute.push_back(create_compute(1));
            EXPECT(compute.back().pso != nullptr);
        }
        compute[2].release();
        compute.erase(compute.begin() + 2);
        compute.push_back(create_compute(1));
        EXPECT(compute.back().pso != nullptr);
    }

    root->Release();

    // The validation layer, when enabled, must have nothing to say about the shared objects' lifetime.
    EXPECT(vktest::validation_error_count() == 0);
    std::printf("pipeline recipe cache stress: ok (%u validation error(s))\n", vktest::validation_error_count());
    return 0;
}
