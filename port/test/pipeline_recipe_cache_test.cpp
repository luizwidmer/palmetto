// The per-process pipeline recipe cache (PALMETTO_PIPELINE_RECIPE_CACHE, docs/pipeline_cache.md): identical and
// differing descriptions are created and released in several orders, so a shared entry's reference count is exercised
// on both the adopt and the release side. Runs on the Vulkan-backed D3D12 device with synthetic GLSL modules; no game
// data, no window and no draws. Set PALMETTO_VK_VALIDATION=1 to check the lifetime under VK_LAYER_KHRONOS_validation:
// a premature destroy of a shared pipeline, module or layout, or a use of a released one, is reported there.
//
// The test turns the switch on itself (before the device is created) so it always exercises the cache; the switch's
// default-off behaviour is the rest of the suite's.
//
// Sections 1 and 2 need no device: the live-pipeline budget's pure policy, and the replay accessor lock order
// (heap_base() must take no lock: it is read from inside execute()'s shared resource lock, and the lazy bind path supplies
// a queued exclusive writer - the freeze docs/memory_growth.md records). Section 6 is the budget's GPU lifetime under
// the validation layer, when PALMETTO_PIPELINE_RECIPE_CACHE=0 PALMETTO_PIPELINE_LIVE_CAP=8 is set for the run.
#include "d3d12_test_shaders.h"
#include "shader_fixture_path.h"
#include "test_temp_dir.h"
#include "vk_test_device.h"
#include "../shims/d3d12_com.h"
#include "../src/shader_library.h"
#include "../src/pipeline_budget.h"  // docs/memory_growth.md: the live-pipeline cap policy
#include "../src/vulkan_backend.h"   // the replay accessor lock order below (VulkanContext, fake Vulkan procedures)

#include <d3d12.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
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
    // The switches are read once, on the first pipeline creation; set them before any device exists. An operator who
    // set PALMETTO_PIPELINE_RECIPE_CACHE explicitly (the budget arm: cache off, PALMETTO_PIPELINE_LIVE_CAP small) keeps
    // that value, so the same binary covers the shared-entry and the deferred/eviction lifetime.
    if (GetEnvironmentVariableA("PALMETTO_PIPELINE_RECIPE_CACHE", nullptr, 0) == 0)
        SetEnvironmentVariableA("PALMETTO_PIPELINE_RECIPE_CACHE", "1");

    // The live-pipeline budget's pure policy (port/src/pipeline_budget.h): a hard cap, LRU eviction, the grace and the
    // disable switch. The GPU side of the same budget is section 6.
    {
        palmetto::PipelineBudget capped(4, 1);
        for (std::uint64_t id = 1; id <= 10; ++id) capped.touch(id);
        EXPECT(capped.live() == 4 && capped.built() == 10 && capped.evicted() == 6);
        EXPECT(capped.least_recently_used() == 7);
        capped.remove(7);
        EXPECT(capped.live() == 3);
        palmetto::PipelineBudget unlimited(0, 1);
        EXPECT(unlimited.touch(1) == 0 && unlimited.live() == 0);
    }

    // The title-page pull coalescer (docs/title_memory_model.md): the include-only policy and the VulkanContext decision
    // path the replay's pull lambda takes. GPU-free: no device, no title memory, no game data. A pull is skipped only
    // when the same submission already copied every byte of it; the trace arm counts the repeat and never skips.
    {
        palmetto::TitlePullWindows windows;
        EXPECT(!windows.covers(0x1000, 0x1000));
        EXPECT(windows.add(0x1000, 0x2000));
        EXPECT(windows.covers(0x1000, 0x2000));
        EXPECT(windows.covers(0x1800, 0x100));
        EXPECT(!windows.covers(0x1800, 0x2000));  // runs past the window: those bytes were not all copied
        EXPECT(!windows.covers(0xff0, 0x20));     // starts before it
        EXPECT(windows.add(0x1800, 0x4000));      // an overlap merges
        EXPECT(windows.size() == 1 && windows.covers(0x1000, 0x4800));
        EXPECT(windows.add(0x5000, 0x1000));      // adjacency merges into one window
        EXPECT(windows.size() == 1 && windows.covers(0x1000, 0x5000));
        EXPECT(!windows.add(0, 0) && !windows.add(~std::uint64_t{0}, 2));
        windows.reset();
        EXPECT(windows.size() == 0);
        // The bound: past kMaxWindows the lowest window is dropped, which only means its bytes are copied again.
        for (std::uint64_t i = 0; i < palmetto::TitlePullWindows::kMaxWindows + 8; ++i)
            EXPECT(windows.add(0x10000 * (i + 1), 0x1000));
        EXPECT(windows.size() == palmetto::TitlePullWindows::kMaxWindows);
        EXPECT(windows.covers(0x10000 * (palmetto::TitlePullWindows::kMaxWindows + 8), 0x1000));
        EXPECT(!windows.covers(0x10000, 0x1000));
    }
    {
        struct Stub {
            static PFN_vkVoidFunction VKAPI_CALL proc(VkDevice, const char* name) {
                (void)name;
                return nullptr;
            }
        };
        _putenv_s("PALMETTO_TITLE_PULL_TRACE", "1");
        _putenv_s("PALMETTO_TITLE_PULL_COALESCE", "");
        {
            palmetto::VulkanContext trace(Stub::proc,
                                          reinterpret_cast<VkDevice>(static_cast<std::uintptr_t>(1)), false);
            EXPECT(trace.title_pull_trace() && !trace.title_pull_coalesce());
            trace.title_pull_begin_submission();
            EXPECT(!trace.title_pull_decide(0x9000, 0x80));
            trace.title_pull_copied(3);
            EXPECT(!trace.title_pull_decide(0x9000, 0x80));  // TRACE counts the repeat, it does not skip
            EXPECT(trace.title_pull_report().find("repeated in submission 1") != std::string::npos);
            trace.title_pull_begin_submission();
            EXPECT(!trace.title_pull_decide(0x9000, 0x80));  // a repeat of the previous submission is counted only
        }
        _putenv_s("PALMETTO_TITLE_PULL_COALESCE", "1");
        {
            palmetto::VulkanContext coalesce(Stub::proc,
                                             reinterpret_cast<VkDevice>(static_cast<std::uintptr_t>(1)), false);
            EXPECT(coalesce.title_pull_coalesce());
            coalesce.title_pull_begin_submission();
            EXPECT(!coalesce.title_pull_decide(0x9000, 0x80));
            EXPECT(coalesce.title_pull_decide(0x9000, 0x80));
            EXPECT(coalesce.title_pull_decide(0x9000, 0x40));
            EXPECT(!coalesce.title_pull_decide(0x9080, 0x80));  // not fully covered: copied
            coalesce.title_pull_begin_submission();
            EXPECT(!coalesce.title_pull_decide(0x9000, 0x80));  // a new submission starts with an empty window
            std::printf("title pull coalescer: ok (policy and VulkanContext decision)\n");
        }
        _putenv_s("PALMETTO_TITLE_PULL_TRACE", "");
        _putenv_s("PALMETTO_TITLE_PULL_COALESCE", "");
    }

    // The replay accessor lock order (docs/memory_growth.md): execute() holds VulkanContext's resource lock shared for
    // the whole replay, and the device callbacks the replay calls read heap_base() from inside it (the device-memory
    // reader/writer). Windows SRW locks are writer-preferring, so a nested shared acquire in heap_base() blocks behind an
    // exclusive writer that is itself waiting for the outer shared lock - the whole-process freeze the default boot hit
    // once the lazy build made set_pipeline_handle() an exclusive writer during the frame loop. The contract is that a
    // replay-side accessor takes no lock. This section proves it with the real VulkanContext and fake Vulkan procedures:
    // no device, no GPU, no game data.
    {
        struct Stub {
            static PFN_vkVoidFunction VKAPI_CALL proc(VkDevice, const char* name) {
                (void)name;
                return nullptr;
            }
        };
        palmetto::VulkanContext context(Stub::proc,
                                        reinterpret_cast<VkDevice>(static_cast<std::uintptr_t>(1)), false);
        context.set_heap_base(0x12340000);
        EXPECT(context.heap_base() == 0x12340000);

        std::atomic<bool> reader_in_body{false};
        std::atomic<bool> writer_queued{false};
        std::atomic<bool> reader_done{false};
        std::uint64_t seen = 0;
        // The reader is execute()'s position: it holds the resource lock shared and reads the accessor from inside.
        std::thread reader([&] {
            context.with_resource_shared_for_test([&] {
                reader_in_body.store(true);
                while (!writer_queued.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                // Let the writer reach the lock and queue for it: the nested shared acquire the old heap_base() made
                // then hung behind it.
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                seen = context.heap_base();
                reader_done.store(true);
            });
        });
        while (!reader_in_body.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        // The writer is the bind path: add_resource() and the lazy build's set_pipeline_handle() take the lock
        // exclusively (the two writer families the pre-fix freeze had queued). It keeps queueing until the read is done,
        // so one is waiting for the whole read.
        std::thread writer([&] {
            palmetto::ResourceBinding binding{};
            writer_queued.store(true);
            for (std::uintptr_t id = 0x5100; !reader_done.load(); ++id) {
                context.add_resource(id, binding);
                context.set_pipeline_handle(id, VK_NULL_HANDLE, true);
            }
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!reader_done.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!reader_done.load()) {
            // Detach rather than join: the point is to report the regression, not to hang the suite on it.
            reader.detach();
            writer.detach();
            std::fprintf(stderr, "FAIL: heap_base() blocked behind a queued exclusive writer (lock-order regression)\n");
            return 1;
        }
        reader.join();
        writer.join();
        EXPECT(seen == 0x12340000);
        std::printf("replay accessor lock order: ok (heap_base() takes no lock under a shared reader)\n");
    }

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

    // 6. The live-pipeline budget's GPU lifetime (docs/memory_growth.md): create more PSOs than the cap (set small for
    // this arm: PALMETTO_PIPELINE_RECIPE_CACHE=0 PALMETTO_PIPELINE_LIVE_CAP=8), then bind every one on a direct command
    // list. A bind past the cap builds a deferred pipeline and evicts another, and a later bind of an evicted one
    // rebuilds it. With PALMETTO_VK_VALIDATION=1 a use of a destroyed pipeline, a double destroy or a rebuilt handle
    // used after its object went is reported by the validation layer, which the last check reads.
    {
        std::vector<PsoRef> budget_psos;
        for (int i = 0; i < 24; ++i) {
            budget_psos.push_back(create(plain));
            EXPECT(budget_psos.back().pso != nullptr);
        }
        ID3D12CommandAllocator* allocator = nullptr;
        EXPECT(SUCCEEDED(layer.device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
            reinterpret_cast<void**>(&allocator))));
        ID3D12GraphicsCommandList* list = nullptr;
        EXPECT(SUCCEEDED(layer.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
                                                         __uuidof(ID3D12GraphicsCommandList),
                                                         reinterpret_cast<void**>(&list))));
        for (auto& pso : budget_psos) list->SetPipelineState(pso.pso);
        // Bind a few a second time: an evicted pipeline state rebuilds its VkPipeline on the rebind.
        for (int i = 0; i < 8; ++i) list->SetPipelineState(budget_psos[static_cast<std::size_t>(i)].pso);
        EXPECT(SUCCEEDED(list->Close()));
        list->Release();
        allocator->Release();
    }

    root->Release();

    // The validation layer, when enabled, must have nothing to say about the shared objects' lifetime.
    EXPECT(vktest::validation_error_count() == 0);
    std::printf("pipeline recipe cache stress: ok (%u validation error(s))\n", vktest::validation_error_count());
    return 0;
}
