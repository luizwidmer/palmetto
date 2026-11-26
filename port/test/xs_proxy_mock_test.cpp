// Xbox-ABI queue and fence proxies (d3d12_xs_device.h) over synthetic COM objects, with no D3D12 runtime and no GPU:
//   - devices that stop at ID3D12Device, Device1, Device3 and Device9: the slots of a missing interface (45/46,
//     50, 75) are past the PC table and stubbed with S_OK / 0 without reaching the device, the others get the real
//     objects behind the proxies;
//   - fences with and without ID3D12Fence1: 11 or 12 public slots, QueryInterface and slot 11 to match;
//   - creation that fails, that returns S_OK with no object, a null IID or output, and an interface the real object
//     has but the proxy does not model;
//   - OpenSharedHandle of a fence (as ID3D12Fence, IUnknown, Fence1), of a non-fence object, of an invalid handle,
//     and with a null output or IID, with the references of every object it touches;
//   - allocation failures in the proxy (operator new, the arrays it unwraps), injected in this executable only;
//   - --fail-virtualalloc / --fail-virtualprotect / --fail-veh: the guard region's setup failing, injected by
//     patching this executable's own import table before the first proxy table is built (one process each).
// Every device, queue, fence and resource here is a mock that records the calls it gets. No game data, no Xbox DLLs.
#include "../shims/d3d12_xs_device.h"

// The frame-event handshake (device slots 78/79/80) that the proxy forwards when the present-driven clock is on
// (docs/peer_port_first_ui.md, the implemented port). Hook-only, so this GPU-free executable stays free of the Vulkan
// layer and d3d12_com.h, exactly as the proxy's translation unit does.
#define PALMETTO_XBOX_PRESENT_HOOK_ONLY
#include "../shims/xbox_present.h"
#undef PALMETTO_XBOX_PRESENT_HOOK_ONLY

// Queue slot 31's measured batch layout and the page-mapping hook the proxy calls (docs/cpu_view_mapping.md). This
// GPU-free executable links no xmem, so the hook is null here unless a check installs its own recorder.
#include "../shims/xmem.h"

// The Vulkan layer's target-view capture (port/src/command_list.h). The proxy reads it by its five words for the
// console ClearRenderTargetView conversion (d3d12_xs_device.cpp, TargetViewWords); these checks keep that mirror tied
// to the layer's definition, and the mock device writes a real one into a CPU descriptor handle below.
#include "../src/command_list.h"

// The proxy's slot-57 branch calls the graphics record decoder and the graphics PSO builder (port/src/graphics_*),
// which the palmetto build links from palmetto_d3d12_com. This executable compiles the proxy source on its own and links no
// runtime library, so the two sources are compiled in here: the branch is then exercised against the real decoder and
// builder, never a second hand-written copy of their contract (their own unit tests are graphics_record_test and
// graphics_pso_test).
#include "../src/graphics_record.cpp"
#include "../src/graphics_pso.cpp"

// The console's depth-stencil + unordered-access pair (docs/3d_textures.md): the Vulkan-backed device translates it
// on the console's placed-creation path, and the shared helper lives in the layer (port/src), not in this executable.
#include "../src/console_texture.h"

#include <d3d12.h>
#include <windows.h>

#include <crtdbg.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <new>
#include <utility>

static_assert(sizeof(palmetto::TargetView) == 24, "the proxy reads the target-view capture as five words");
static_assert(offsetof(palmetto::TargetView, resource) == 0, "target view: resource id first");
static_assert(offsetof(palmetto::TargetView, layer_count) == 16, "target view: layer count is the fourth word");

namespace {

const char* g_label = "xs_proxy_mock";

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "%s line %d: %s\n", g_label, __LINE__, #condition); return 1; } \
} while (0)

// ---- Allocation failures: this executable's global operator new fails the next g_fail_news calls ----

int g_fail_news = 0;     // single-threaded: armed right before the call it is meant for
int g_failed_news = 0;   // allocations refused so far

}  // namespace

// The annotations are vcruntime_new.h's, so /analyze sees the same contract as for the CRT's own operator new.
_NODISCARD _Ret_notnull_ _Post_writable_byte_size_(size) _VCRT_ALLOCATOR
void* __CRTDECL operator new(std::size_t size) {
    if (g_fail_news > 0) {
        --g_fail_news;
        ++g_failed_news;
        throw std::bad_alloc();
    }
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
_NODISCARD _Ret_maybenull_ _Success_(return != NULL) _Post_writable_byte_size_(size) _VCRT_ALLOCATOR
void* __CRTDECL operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}
void __CRTDECL operator delete(void* p) noexcept { std::free(p); }
void __CRTDECL operator delete(void* p, std::size_t) noexcept { std::free(p); }
void __CRTDECL operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }

namespace {

namespace xs = rt::d3d12_xs;

// Console fence slot 12 (docs/fence_slot12.md): the proxy answers with the address of the fence's completed-value
// cell and, when the value has not reached the target the caller passed, asks the registered watcher to bring it
// there. This is the GPU-free stand-in for the Vulkan layer's fence dispatcher: it records the request and the test
// moves the value (the mock fence reaches a target through mock_call).
unsigned g_watch_calls = 0;
std::uint64_t g_watch_target = 0;
std::atomic<std::uint64_t>* g_watched_cell = nullptr;
void test_fence_watcher(IUnknown* real_fence, const std::shared_ptr<std::atomic<std::uint64_t>>& cell,
                        std::uint64_t target) {
    (void)real_fence;
    ++g_watch_calls;
    g_watch_target = target;
    g_watched_cell = cell.get();
}

// ---- Mocks ----

enum class Kind { device, queue, fence, resource, allocator, command_list, heap, root_signature, pipeline_state, command_signature };

constexpr unsigned kMockSlots = 128;
constexpr std::uint64_t kFenceHandle = 0x1000, kResourceHandle = 0x2000;  // the handles the mock device opens
constexpr std::uint64_t kCompletedValue = 7;                            // what a mock fence's slot 8 returns
// {5d1e0c4a-2b7f-4e61-9a3d-8c4b2f1e6a70}: an interface the mock queue implements that the proxy does not model.
constexpr GUID kExtraIid{0x5d1e0c4a, 0x2b7f, 0x4e61, {0x9a, 0x3d, 0x8c, 0x4b, 0x2f, 0x1e, 0x6a, 0x70}};
// What the mock real queue's GetDesc answers: synthetic values, non-zero in every field, so a stale title buffer (the
// old forwarding left it untouched) or a descriptor written in the wrong place cannot match it by accident.
constexpr D3D12_COMMAND_QUEUE_DESC kMockQueueDesc{D3D12_COMMAND_LIST_TYPE_COPY, 0x1d2c,
                                                  D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT, 0x5a};

struct Mock {
    void** vtable;
    Kind kind;
    long refs;
    int device_level;        // device: highest ID3D12DeviceN it answers (0 = ID3D12Device only)
    int list_level;          // command_list: 0=base (60 slots), 7=GCL7 (81 slots), 8=GCL8 (82 slots)
    bool fence1;             // fence: answers ID3D12Fence1; device: the fences it makes do
    std::uint64_t fence_value;  // fence: GetCompletedValue's answer; a Signal raises it
    bool allocator_tag;      // allocator: answers kPalmettoCommandAllocatorTag; device: the allocators it makes do
    bool create_null;        // device: creation returns create_result with no object
    HRESULT create_result;   // device: what creation returns (S_OK makes the object)
    Mock* last_child;        // device: the object its last creation or open made
    // Device slot 25 (PC GetResourceAllocationInfo): how many descriptions the "real" method was asked to size and the
    // flags of the last one, so the slot-25 stand-in's re-query without ALLOW_UNORDERED_ACCESS is visible.
    unsigned allocation_queries;
    D3D12_RESOURCE_FLAGS allocation_flags;
    unsigned calls[kMockSlots];
    std::uint64_t args[kMockSlots][5];
    std::uint64_t array[kMockSlots][4];  // device: the first entries of the array argument, as received
    // Queue slot 18 (GetDesc): the two arguments the real method was called with, as the proxy's conversion passed
    // them. The test compares them with the pointers it knows: the hidden buffer must be the title's, the object the
    // mock queue itself.
    std::uint64_t get_desc_hidden;
    std::uint64_t get_desc_self;
    // Device slot 8 (CreateCommandQueue): the description the PC method was handed. It is the proxy's own copy, not
    // the title's pointer (slot 48 copies the 16-byte console prefix and masks its Flags), so the test reads the
    // contents rather than the pointer's identity.
    D3D12_COMMAND_QUEUE_DESC queue_desc;
    // Device slot 12 (CreateCommandList): the IID the PC method was asked for, by value. Slot 73 asks the host for the
    // base GraphicsCommandList interface and passes its own copy of the title's IID, so the pointer is not the title's.
    GUID list_iid;
    // Device slot 10 (PC CreateGraphicsPipelineState): the description the proxy's slot-57 conversion built, copied at
    // call time. It is the proxy's own structure, so the test reads its contents rather than a pointer's identity.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics_desc;
    // Device slot 41 (PC CreateCommandSignature): the description the proxy's console argument mapping handed the host,
    // copied at call time, and the pointer it was handed at. A translated console description arrives as the proxy's own
    // copy (the pointer differs from the title's); one the measurement does not name arrives unchanged.
    D3D12_COMMAND_SIGNATURE_DESC signature_desc;
    const void* signature_desc_pointer;
    std::uint64_t signature_root;
    D3D12_INDIRECT_ARGUMENT_DESC signature_args[8];
    unsigned signature_arg_count;
    // Device slot 27 (PC CreateCommittedResource): what the proxy's slot-127 conversion (the console's
    // CreateCommittedResource with a barrier layout in the initial state's place) hands the host, copied at call time,
    // and whether the host's out write reached the title's pointer.
    D3D12_HEAP_PROPERTIES committed_props;
    D3D12_RESOURCE_DESC committed_desc;
    D3D12_HEAP_FLAGS committed_heap_flags;
    D3D12_RESOURCE_STATES committed_state;
    bool committed_out_written;
    // Command list: the real PC list's own lifecycle. The proxy tracks the recording state itself and reopens a
    // closed list before a forwarded recording call (docs/list_lifecycle.md), so the mock models the two slots that
    // define that state: Close (9) leaves the real list closed, Reset (10) reopens it and reports reset_result
    // (E_FAIL injects the reopen failure path). list_events is the order the command-list slots were called in, to
    // prove the Reset comes before the recording call it reopened for.
    bool list_closed;
    HRESULT reset_result;
    unsigned list_events[kMockSlots];
    unsigned list_event_count;
    // Command list slot 80 (GCL7::Barrier): the groups the proxy forwarded, copied at call time. The proxy translates a
    // console group whose texture barriers carry no resource into global barriers (d3d12_xs_device.cpp), so the test
    // reads what the "real" method was actually given, not what the console call passed.
    unsigned barrier_group_count;
    D3D12_BARRIER_GROUP barrier_groups[4];
    D3D12_GLOBAL_BARRIER barrier_globals[8];
    D3D12_TEXTURE_BARRIER barrier_textures[8];
    // Device slot 17 (PC CreateConstantBufferView): what the proxy's console slot-70 conversion handed the host, copied
    // at call time, the destination it wrote and how many times it was reached. The real layer writes a 32-byte
    // descriptor at the destination; the mock writes a recognisable mark there, so the test can prove that the write
    // landed in the title's descriptor slot and not somewhere else.
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_desc;
    std::uint64_t cbv_dest;
    unsigned cbv_calls;
    // Device slot 18 (PC CreateShaderResourceView): the layer's own T# path. The pairing publication
    // (PALMETTO_SLOT70_PAIR) hands the paired resource and a view description built from the publication's resource
    // description to the real device; the mock records both and writes a marker at the destination.
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc;
    std::uint64_t srv_dest;
    std::uint64_t srv_resource;
    unsigned srv_calls;
    // PC command-list slot 62 (OMSetDepthBounds): the floats the proxy's float-shaped console slot-70 entry forwarded
    // (docs/bottom_of_pipe.md). The generic integer thunk cannot carry them, so the test reads them here.
    unsigned depth_bounds_calls;
    float depth_bounds_min;
    float depth_bounds_max;
};

Mock g_pool[256];
unsigned g_pool_used = 0;

void** mock_vtable();

Mock* new_mock(Kind kind) {
    if (g_pool_used == std::size(g_pool)) {
        std::fprintf(stderr, "%s: mock pool exhausted\n", g_label);
        std::exit(1);
    }
    Mock* m = &g_pool[g_pool_used++];
    std::memset(m, 0, sizeof(*m));
    m->vtable = mock_vtable();
    m->kind = kind;
    return m;
}

bool mock_has(const Mock* m, REFIID iid) {
    if (IsEqualIID(iid, __uuidof(IUnknown)) || IsEqualIID(iid, __uuidof(ID3D12Object))) return true;
    if (m->kind == Kind::device) {
        if (IsEqualIID(iid, __uuidof(ID3D12Device))) return true;
        const IID levels[] = {__uuidof(ID3D12Device1), __uuidof(ID3D12Device2), __uuidof(ID3D12Device3),
                              __uuidof(ID3D12Device4), __uuidof(ID3D12Device5), __uuidof(ID3D12Device6),
                              __uuidof(ID3D12Device7), __uuidof(ID3D12Device8), __uuidof(ID3D12Device9),
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
                              __uuidof(ID3D12Device10),
#endif
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
                              __uuidof(ID3D12Device11),
#endif
#ifdef __ID3D12Device12_INTERFACE_DEFINED__
                              __uuidof(ID3D12Device12),
#endif
#ifdef __ID3D12Device13_INTERFACE_DEFINED__
                              __uuidof(ID3D12Device13),
#endif
#ifdef __ID3D12Device14_INTERFACE_DEFINED__
                              __uuidof(ID3D12Device14),
#endif
        };
        for (int i = 0; i < m->device_level && i < static_cast<int>(std::size(levels)); ++i)
            if (IsEqualIID(iid, levels[i])) return true;
        return false;
    }
    if (IsEqualIID(iid, __uuidof(ID3D12DeviceChild)) || IsEqualIID(iid, __uuidof(ID3D12Pageable))) return true;
    switch (m->kind) {
        case Kind::queue: return IsEqualIID(iid, __uuidof(ID3D12CommandQueue)) || IsEqualIID(iid, kExtraIid);
        case Kind::fence:
            return IsEqualIID(iid, __uuidof(ID3D12Fence)) || (m->fence1 && IsEqualIID(iid, __uuidof(ID3D12Fence1)));
        case Kind::resource: return IsEqualIID(iid, __uuidof(ID3D12Resource));
        case Kind::heap: return IsEqualIID(iid, __uuidof(ID3D12DescriptorHeap));
        case Kind::root_signature: return IsEqualIID(iid, __uuidof(ID3D12RootSignature));
        case Kind::pipeline_state: return IsEqualIID(iid, __uuidof(ID3D12PipelineState));
        case Kind::command_list:
            if (IsEqualIID(iid, __uuidof(ID3D12CommandList)) ||
                IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList))) return true;
#ifdef __ID3D12GraphicsCommandList7_INTERFACE_DEFINED__
            if (m->list_level >= 7 && (IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList7)) ||
                                       IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList6)) ||
                                       IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList5)) ||
                                       IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList4)) ||
                                       IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList3)) ||
                                       IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList2)) ||
                                       IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList1))))
                return true;
#endif
            return false;
        case Kind::allocator:
            return IsEqualIID(iid, __uuidof(ID3D12CommandAllocator)) ||
                   (m->allocator_tag && IsEqualIID(iid, xs::kPalmettoCommandAllocatorTag));
        default: return false;
    }
}

HRESULT mock_query(Mock* m, const IID* riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid || !mock_has(m, *riid)) return E_NOINTERFACE;
    *out = m;
    ++m->refs;
    return S_OK;
}

// A device creating (or opening) a `kind` object as `riid`: S_FALSE without an output, as the PC runtime answers a
// capability check; the device's create_result and create_null otherwise.
HRESULT mock_create(Mock* device, Kind kind, std::uint64_t riid, std::uint64_t out_word) {
    auto** out = reinterpret_cast<void**>(out_word);
    if (!out) return S_FALSE;
    *out = nullptr;
    if (device->create_null || FAILED(device->create_result)) return device->create_result;
    Mock* child = new_mock(kind);
    child->fence1 = device->fence1;
    child->allocator_tag = device->allocator_tag;
    if (kind == Kind::fence) child->fence_value = kCompletedValue;
    device->last_child = child;
    return mock_query(child, reinterpret_cast<const IID*>(riid), out);
}

void copy_array(Mock* m, unsigned slot, std::uint64_t objects, std::uint64_t count) {
    const auto* in = reinterpret_cast<const std::uint64_t*>(objects);
    for (std::uint32_t i = 0; in && i < static_cast<std::uint32_t>(count) && i < 4; ++i) m->array[slot][i] = in[i];
}

// `words` are the call's first five words (a1..a5), copied into the thunk's own frame.
std::uint64_t mock_call(Mock* m, unsigned slot, const std::uint64_t* words) {
    const std::uint64_t a1 = words[0], a2 = words[1], a3 = words[2], a4 = words[3];
    if (m->kind == Kind::device) {
        switch (slot) {
            case 8:  // CreateCommandQueue(desc, riid, out)
                if (a1) std::memcpy(&m->queue_desc, reinterpret_cast<const void*>(a1), sizeof(m->queue_desc));
                return static_cast<std::uint32_t>(mock_create(m, Kind::queue, a2, a3));
            case 9: return static_cast<std::uint32_t>(mock_create(m, Kind::allocator, a2, a3));  // CreateCmdAllocator
            case 14: return static_cast<std::uint32_t>(mock_create(m, Kind::heap, a2, a3));       // CreateDescriptorHeap
            case 17: {  // CreateConstantBufferView(desc, destination): the console slot-70 conversion's PC target
                ++m->cbv_calls;
                m->cbv_dest = a2;
                if (a1) m->cbv_desc = *reinterpret_cast<const D3D12_CONSTANT_BUFFER_VIEW_DESC*>(a1);
                if (a2) {
                    // The real layer writes kDescriptorSize = 32 bytes of a V# at the destination; this is the mock's
                    // own stand-in, non-zero in every word, so a write to a different slot cannot match it.
                    const std::uint32_t mark[8] = {0x17u, m->cbv_desc.SizeInBytes, 0x1717u, 0x17171717u,
                                                   0x17171717u, 0x17171717u, 0x17171717u, 0x17171717u};
                    std::memcpy(reinterpret_cast<void*>(a2), mark, sizeof(mark));
                }
                return static_cast<std::uint64_t>(static_cast<std::uint32_t>(S_OK));
            }
            case 75: return static_cast<std::uint32_t>(mock_create(m, Kind::queue, a3, a4));   // CreateCommandQueue1
            case 36: return static_cast<std::uint32_t>(mock_create(m, Kind::fence, a3, a4));   // CreateFence
            case 32:                                                                          // OpenSharedHandle
                if (a1 == kFenceHandle) return static_cast<std::uint32_t>(mock_create(m, Kind::fence, a2, a3));
                if (a1 == kResourceHandle) return static_cast<std::uint32_t>(mock_create(m, Kind::resource, a2, a3));
                return static_cast<std::uint32_t>(E_INVALIDARG);
            case 34: case 35: case 46: copy_array(m, slot, a2, a1); break;  // (count, objects, ...)
            case 45: copy_array(m, slot, a1, a3); break;                    // (fences, values, count, ...)
            case 50: copy_array(m, slot, a3, a2); break;                    // (flags, count, objects, fence, value)
            case 20: {  // CreateRenderTargetView(resource, desc, destination): write the layer's capture, as it does
                if (!a3) return S_OK;
                palmetto::TargetView view{};
                view.resource = static_cast<palmetto::ResourceId>(a1);
                view.layer_count = a1 ? 1u : 0u;  // a null resource is a null view: no resource, no layers
                std::memcpy(reinterpret_cast<void*>(a3), &view, sizeof(view));
                return S_OK;
            }
            default: break;
        }
        return S_OK;
    }
    if (m->kind == Kind::fence && slot == 8) return m->fence_value;  // GetCompletedValue
    if (m->kind == Kind::fence && slot == 10) {                      // Signal(value)
        if (a1 > m->fence_value) m->fence_value = a1;
        return S_OK;
    }
    if (m->kind == Kind::queue && slot == 14 && a1) {  // Signal(fence, value): the real fence is the first argument
        auto* const fence = reinterpret_cast<Mock*>(a1);
        if (fence->kind == Kind::fence && a2 > fence->fence_value) fence->fence_value = a2;
    }
    if (m->kind == Kind::queue && slot == 10) copy_array(m, slot, a2, a1);  // ExecuteCommandLists(count, lists)
    if (m->kind == Kind::command_list && slot == 28) copy_array(m, slot, a2, a1);  // SetDescriptorHeaps(count, heaps)
    if (m->kind == Kind::command_list && slot == 80) {  // Barrier(NumBarrierGroups, pBarrierGroups)
        m->barrier_group_count = static_cast<unsigned>(a1);
        const auto* groups = reinterpret_cast<const D3D12_BARRIER_GROUP*>(a2);
        for (unsigned g = 0; groups && g < m->barrier_group_count && g < std::size(m->barrier_groups); ++g) {
            m->barrier_groups[g] = groups[g];
            if (groups[g].Type == D3D12_BARRIER_TYPE_GLOBAL && groups[g].pGlobalBarriers) {
                for (unsigned i = 0; i < groups[g].NumBarriers && i < std::size(m->barrier_globals); ++i)
                    m->barrier_globals[i] = groups[g].pGlobalBarriers[i];
            } else if (groups[g].Type == D3D12_BARRIER_TYPE_TEXTURE && groups[g].pTextureBarriers) {
                for (unsigned i = 0; i < groups[g].NumBarriers && i < std::size(m->barrier_textures); ++i)
                    m->barrier_textures[i] = groups[g].pTextureBarriers[i];
            }
        }
    }
    if (m->kind == Kind::command_list && slot == 9) {  // Close: the PC list stops recording
        m->list_closed = true;
        return static_cast<std::uint32_t>(S_OK);
    }
    if (m->kind == Kind::command_list && slot == 10) {  // Reset(allocator, state): starts a recording, or reports
        if (FAILED(m->reset_result)) return static_cast<std::uint32_t>(m->reset_result);
        m->list_closed = false;
        return static_cast<std::uint32_t>(S_OK);
    }
    return S_OK;
}

template <unsigned Slot>
std::uint64_t WINAPI mock_slot(Mock* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3, std::uint64_t a4,
                               std::uint64_t a5) {
    if constexpr (Slot == 0) {
        return static_cast<std::uint32_t>(mock_query(self, reinterpret_cast<const IID*>(a1),
                                                     reinterpret_cast<void**>(a2)));
    } else if constexpr (Slot == 1) {
        return static_cast<std::uint64_t>(++self->refs);
    } else if constexpr (Slot == 2) {
        return static_cast<std::uint64_t>(--self->refs);
    } else {
        // A caller may pass fewer words than this thunk declares (OpenSharedHandle passes three), so its frame
        // has no room past the four register homes. The words are copied into this frame and passed by pointer:
        // forwarding them as stack arguments let MSVC tail-call mock_call and write into the caller's saved
        // registers (an optimized build corrupted the test's own locals that way).
        ++self->calls[Slot];
        if (self->kind == Kind::command_list && self->list_event_count < std::size(self->list_events))
            self->list_events[self->list_event_count++] = Slot;
        const std::uint64_t words[] = {a1, a2, a3, a4, a5};
        std::memcpy(self->args[Slot], words, sizeof(words));
        return mock_call(self, Slot, words);
    }
}

// PC ID3D12Device::CreateCommandList takes six words after `this`, one more than the generic mock thunk declares, so
// it gets its own entry in the table: the out pointer is a6 (first stack argument on x64) and has to be written.
HRESULT WINAPI mock_create_command_list(Mock* self, std::uint64_t node_mask, std::uint64_t type,
                                        std::uint64_t allocator, std::uint64_t initial_state, const IID* riid,
                                        void** out) {
    ++self->calls[12];
    self->args[12][0] = node_mask;
    self->args[12][1] = type;
    self->args[12][2] = allocator;
    self->args[12][3] = initial_state;
    self->args[12][4] = reinterpret_cast<std::uint64_t>(riid);
    if (riid) self->list_iid = *riid;  // by value: the proxy's PC IID is a copy, not the title's pointer
    if (!out) return S_FALSE;
    *out = nullptr;
    if (self->create_null || FAILED(self->create_result)) return self->create_result;
    Mock* child = new_mock(Kind::command_list);
    child->list_level = self->list_level;
    self->last_child = child;
    return mock_query(child, riid, out);
}

// PC ID3D12Device::CreateRootSignature (slot 16): the proxy forwards it unchanged and remembers the object it made, so
// slot 57 can prove that a record's +0x00 root signature is one this device handed out. The mock makes a
// root-signature object and hands it out, as the PC device does. The table is shared by every mock kind, and the
// console queue's methods are shifted onto PC slots 14..18, so a queue's slot 16 stays the generic forwarded call.
HRESULT WINAPI mock_create_root_signature(Mock* self, UINT node_mask, const void* blob, SIZE_T size, const IID* riid,
                                          void** out) {
    if (self->kind != Kind::device)
        return static_cast<HRESULT>(static_cast<std::uint32_t>(mock_slot<16>(
            self, node_mask, reinterpret_cast<std::uint64_t>(blob), size, reinterpret_cast<std::uint64_t>(riid),
            reinterpret_cast<std::uint64_t>(out))));
    ++self->calls[16];
    self->args[16][0] = node_mask;
    self->args[16][1] = reinterpret_cast<std::uint64_t>(blob);
    self->args[16][2] = size;
    self->args[16][3] = reinterpret_cast<std::uint64_t>(riid);
    self->args[16][4] = reinterpret_cast<std::uint64_t>(out);
    return mock_create(self, Kind::root_signature, reinterpret_cast<std::uint64_t>(riid),
                       reinterpret_cast<std::uint64_t>(out));
}

// PC ID3D12Device::CreateCommandSignature (slot 41): the proxy's console argument mapping hands the host either the
// title's own description or its bounded copy with the console-only argument type translated (docs/command_signature.md).
// The mock records both and refuses an argument type it does not know, as the PC runtime does, so a console type the
// measurement does not name cannot pass as a silent success.
HRESULT WINAPI mock_create_command_signature(Mock* self, const D3D12_COMMAND_SIGNATURE_DESC* desc,
                                             std::uint64_t root_signature, const IID* riid, void** out) {
    if (self->kind != Kind::device)
        return static_cast<HRESULT>(static_cast<std::uint32_t>(mock_slot<41>(
            self, reinterpret_cast<std::uint64_t>(desc), root_signature, reinterpret_cast<std::uint64_t>(riid),
            reinterpret_cast<std::uint64_t>(out), 0)));
    ++self->calls[41];
    self->args[41][0] = reinterpret_cast<std::uint64_t>(desc);
    self->args[41][1] = root_signature;
    self->args[41][2] = reinterpret_cast<std::uint64_t>(riid);
    self->args[41][3] = reinterpret_cast<std::uint64_t>(out);
    self->signature_desc_pointer = desc;
    self->signature_root = root_signature;
    self->signature_arg_count = 0;
    if (desc) {
        self->signature_desc = *desc;
        for (UINT i = 0; desc->pArgumentDescs && i < desc->NumArgumentDescs && i < std::size(self->signature_args); ++i)
            self->signature_args[self->signature_arg_count++] = desc->pArgumentDescs[i];
        // The PC's argument types are 0..11 (11 is ExecuteIndirect tier 1.1's incrementing constant); anything else is
        // what the host refuses with E_INVALIDARG.
        for (unsigned i = 0; i < self->signature_arg_count; ++i) {
            std::uint32_t type = 0;
            std::memcpy(&type, &self->signature_args[i].Type, sizeof(type));
            if (type > 11) {
                if (out) *out = nullptr;
                return E_INVALIDARG;
            }
        }
    }
    if (!out) return S_FALSE;
    *out = nullptr;
    if (self->create_null || FAILED(self->create_result)) return self->create_result;
    Mock* child = new_mock(Kind::command_signature);
    child->refs = 1;
    self->last_child = child;
    *out = child;
    return S_OK;
}

// PC ID3D12Device::CreateGraphicsPipelineState (slot 10): the proxy's slot-57 conversion hands it the description it
// built. The mock copies that description and hands out a pipeline-state object, like the PC device; create_result and
// create_null (as on creation) choose the answer, so the test can check an honest failed HRESULT comes back unchanged.
// The queue proxy also forwards onto its real object's slot 10, so another mock kind keeps the generic thunk.
HRESULT WINAPI mock_create_graphics_pipeline_state(Mock* self, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
                                                   const IID* riid, void** out) {
    if (self->kind != Kind::device)
        return static_cast<HRESULT>(static_cast<std::uint32_t>(mock_slot<10>(
            self, reinterpret_cast<std::uint64_t>(desc), reinterpret_cast<std::uint64_t>(riid),
            reinterpret_cast<std::uint64_t>(out), 0, 0)));
    ++self->calls[10];
    self->args[10][0] = reinterpret_cast<std::uint64_t>(desc);
    self->args[10][1] = reinterpret_cast<std::uint64_t>(riid);
    self->args[10][2] = reinterpret_cast<std::uint64_t>(out);
    if (desc) self->graphics_desc = *desc;
    return mock_create(self, Kind::pipeline_state, reinterpret_cast<std::uint64_t>(riid),
                       reinterpret_cast<std::uint64_t>(out));
}

// PC ID3D12Device::CreateCommittedResource (slot 27): the proxy's slot-127 conversion (the console's
// CreateCommittedResource with a barrier layout in the initial state's place) hands the host a translated description.
// The mock records what it was given and hands a resource out, as the PC device does; the generic thunk cannot,
// because its out pointer is the seventh argument.
HRESULT WINAPI mock_create_committed_resource(Mock* self, const D3D12_HEAP_PROPERTIES* props, D3D12_HEAP_FLAGS flags,
                                              const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
                                              const D3D12_CLEAR_VALUE* clear, const IID* riid, void** out) {
    if (self->kind != Kind::device)
        return static_cast<HRESULT>(static_cast<std::uint32_t>(mock_slot<27>(
            self, reinterpret_cast<std::uint64_t>(props), static_cast<std::uint64_t>(flags),
            reinterpret_cast<std::uint64_t>(desc), static_cast<std::uint64_t>(state),
            reinterpret_cast<std::uint64_t>(clear))));
    ++self->calls[27];
    self->committed_props = props ? *props : D3D12_HEAP_PROPERTIES{};
    self->committed_desc = desc ? *desc : D3D12_RESOURCE_DESC{};
    self->committed_heap_flags = flags;
    self->committed_state = state;
    if (!out) return S_FALSE;
    *out = reinterpret_cast<void*>(0x1270);
    self->committed_out_written = true;
    return S_OK;
}

// PC ID3D12CommandQueue::GetDesc returns D3D12_COMMAND_QUEUE_DESC by value, so MSVC's convention hands that method the
// hidden return buffer in RCX and the object in RDX - the opposite of the title's (queue, buffer) call, which is why
// the proxy has a conversion for slot 18 (measured: forwarded the generic way, the descriptor went into the front of
// the real queue object and the title kept reading its old buffer). This mock is the "real" method: it records the two
// pointers it was called with and writes the descriptor to the hidden buffer, as the PC method would.
void WINAPI mock_queue_get_desc(void* hidden, void* object) {
    auto* const self = reinterpret_cast<Mock*>(object);
    ++self->calls[18];
    self->get_desc_hidden = reinterpret_cast<std::uint64_t>(hidden);
    self->get_desc_self = reinterpret_cast<std::uint64_t>(object);
    std::memcpy(hidden, &kMockQueueDesc, sizeof(kMockQueueDesc));
}

// PC ID3D12Device::GetResourceAllocationInfo returns D3D12_RESOURCE_ALLOCATION_INFO by value, so MSVC's convention hands
// that method `this` in RCX and the hidden return buffer in RDX (the opposite of the title's console order, which puts the
// buffer first - that is why the proxy has a conversion for slot 25). The PC runtime refuses a texture that allows both
// depth-stencil and unordered access with D3D12's invalid allocation (SizeInBytes = UINT64_MAX) and refuses a zero-width
// description; everything else gets a synthetic 64 KiB-aligned size. The slot-25 stand-in's re-query without
// ALLOW_UNORDERED_ACCESS is visible through allocation_queries and allocation_flags.
void* WINAPI mock_get_resource_allocation_info(Mock* self, void* hidden, UINT mask, UINT count,
                                               const D3D12_RESOURCE_DESC* descs) {
    auto* const info = reinterpret_cast<D3D12_RESOURCE_ALLOCATION_INFO*>(hidden);
    ++self->calls[25];
    self->args[25][0] = mask;
    self->args[25][1] = count;
    bool refused = !descs || !count || !descs[0].Width;
    if (!refused) {
        ++self->allocation_queries;
        self->allocation_flags = descs[0].Flags;
        refused = descs[0].Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
                  (descs[0].Flags & (D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) ==
                      (D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    }
    info->SizeInBytes = refused ? UINT64_MAX : 0x10000;
    info->Alignment = refused ? 0 : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    return hidden;
}

// PC ID3D12Device::CreateShaderResourceView (slot 18), the layer's T# encoder: the console slot-70 pairing publication
// hands it the paired resource and the view description it built, and the mock writes a marker at the destination.
void WINAPI mock_create_srv(Mock* self, ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
                            D3D12_CPU_DESCRIPTOR_HANDLE destination) {
    ++self->srv_calls;
    self->srv_resource = reinterpret_cast<std::uint64_t>(resource);
    self->srv_dest = destination.ptr;
    if (desc) self->srv_desc = *desc;
    if (destination.ptr) {
        const std::uint32_t mark[8] = {0x18u, static_cast<std::uint32_t>(self->srv_desc.ViewDimension),
                                       self->srv_desc.Format, 0x18181818u,
                                       0x18181818u, 0x18181818u, 0x18181818u, 0x18181818u};
        std::memcpy(reinterpret_cast<void*>(destination.ptr), mark, sizeof(mark));
    }
}

// The mock vtable is shared by every mock kind, and slot 18 is both the device's CreateShaderResourceView and the queue's
// struct-returning GetDesc. Which one it is depends on the object the call arrives on.
void WINAPI mock_slot18(void* object, void* a, void* b, void* c) {
    if (reinterpret_cast<Mock*>(object)->kind == Kind::device) {
        mock_create_srv(reinterpret_cast<Mock*>(object), reinterpret_cast<ID3D12Resource*>(a),
                        reinterpret_cast<const D3D12_SHADER_RESOURCE_VIEW_DESC*>(b),
                        D3D12_CPU_DESCRIPTOR_HANDLE{reinterpret_cast<SIZE_T>(c)});
        return;
    }
    mock_queue_get_desc(object, a);  // (hidden return buffer, queue object)
}

template <std::size_t... I>
void fill_mock_vtable(void** vtable, std::index_sequence<I...>) {
    ((vtable[I] = reinterpret_cast<void*>(&mock_slot<static_cast<unsigned>(I)>)), ...);
}

// PC ID3D12GraphicsCommandList1::OMSetDepthBounds (slot 62): the proxy's console slot-70 float-shaped entry forwards
// its two XMM words here (docs/bottom_of_pipe.md). This table entry replaces the generic thunk for slot 62, which no
// other mock check uses.
void WINAPI mock_om_set_depth_bounds(Mock* self, float minimum, float maximum) {
    ++self->calls[62];
    ++self->depth_bounds_calls;
    self->depth_bounds_min = minimum;
    self->depth_bounds_max = maximum;
}

void** mock_vtable() {
    static void* table[kMockSlots];
    static const bool filled = [] {
        fill_mock_vtable(table, std::make_index_sequence<kMockSlots>{});
        table[10] = reinterpret_cast<void*>(&mock_create_graphics_pipeline_state);  // PC CreateGraphicsPipelineState
        table[12] = reinterpret_cast<void*>(&mock_create_command_list);
        table[16] = reinterpret_cast<void*>(&mock_create_root_signature);  // PC CreateRootSignature
        table[25] = reinterpret_cast<void*>(&mock_get_resource_allocation_info);  // PC GetResourceAllocationInfo
        table[27] = reinterpret_cast<void*>(&mock_create_committed_resource);  // PC CreateCommittedResource
        table[41] = reinterpret_cast<void*>(&mock_create_command_signature);  // PC CreateCommandSignature
        table[18] = reinterpret_cast<void*>(&mock_slot18);  // GetDesc for a queue, CreateShaderResourceView for a device
        table[62] = reinterpret_cast<void*>(&mock_om_set_depth_bounds);  // PC OMSetDepthBounds (console list 70's target)
        return true;
    }();
    (void)filled;
    return table;
}

// ---- Calls by slot, the way a title compiled against the console headers makes them ----

using SlotFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                      std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                      std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);

std::uint64_t raw(const void* object, unsigned slot, std::uint64_t a1 = 0, std::uint64_t a2 = 0,
                  std::uint64_t a3 = 0, std::uint64_t a4 = 0, std::uint64_t a5 = 0, std::uint64_t a6 = 0,
                  std::uint64_t a7 = 0, std::uint64_t a8 = 0, std::uint64_t a9 = 0, std::uint64_t a10 = 0) {
    void* const* vtable = *reinterpret_cast<void* const* const*>(object);
    return reinterpret_cast<SlotFn>(vtable[slot])(const_cast<void*>(object), a1, a2, a3, a4, a5, a6, a7, a8, a9, a10,
                                                  0, 0, 0, 0);
}

HRESULT hr_of(std::uint64_t value) {
    return static_cast<HRESULT>(static_cast<std::uint32_t>(value));
}

template <class T>
std::uint64_t word(const T* p) {
    return reinterpret_cast<std::uint64_t>(p);
}

const IID kQueueIid = __uuidof(ID3D12CommandQueue);
const IID kFenceIid = __uuidof(ID3D12Fence);
const IID kFence1Iid = __uuidof(ID3D12Fence1);
const IID kUnknownIid = __uuidof(IUnknown);
const IID kResourceIid = __uuidof(ID3D12Resource);
const IID kDevice1Iid = __uuidof(ID3D12Device1);
const D3D12_COMMAND_QUEUE_DESC kQueueDesc{};

// A device proxy over a new mock device. The proxy takes the mock's one reference.
void* make_device(Mock*& mock, int level, bool fence1, bool allocator_tag = true) {
    mock = new_mock(Kind::device);
    mock->device_level = level;
    mock->fence1 = fence1;
    mock->allocator_tag = allocator_tag;
    mock->refs = 1;
    void* device = nullptr;
    if (FAILED(xs::create_device_proxy(reinterpret_cast<ID3D12Device*>(mock), __uuidof(ID3D12Device), &device)))
        return nullptr;
    return device;
}

void* make_queue(void* device) {
    void* queue = nullptr;
    return SUCCEEDED(hr_of(raw(device, 8, word(&kQueueDesc), word(&kQueueIid), word(&queue)))) ? queue : nullptr;
}

void* make_fence(void* device, const IID& iid = kFenceIid) {
    void* fence = nullptr;
    return SUCCEEDED(hr_of(raw(device, 36, 0, 0, word(&iid), word(&fence)))) ? fence : nullptr;
}

unsigned long release(void* object) {
    return static_cast<unsigned long>(raw(object, 2));
}

// ---- 1. Device interface levels ----

int check_levels() {
    struct Level { int level; unsigned slots; } const levels[] = {{0, 44}, {1, 47}, {3, 51}, {9, 76}};
    for (const Level& l : levels) {
        Mock* mock = nullptr;
        void* device = make_device(mock, l.level, true);
        EXPECT(device && xs::device_proxy_slot_count(device) == l.slots);
        void* queue = make_queue(device);
        void* fence = make_fence(device);
        EXPECT(xs::is_queue_proxy(queue) && xs::is_fence_proxy(fence));
        const auto real_queue = word(xs::queue_proxy_real_queue(queue));
        const auto real_fence = word(xs::fence_proxy_real_fence(fence));
        EXPECT(real_queue && real_fence && real_queue != word(queue) && real_fence != word(fence));
        // What every residency call gets, with the proxies in it replaced by their real objects.
        const std::uint64_t objects[4] = {word(queue), word(fence), real_queue, 0};
        const std::uint64_t unwrapped[4] = {real_queue, real_fence, real_queue, 0};
        const std::uint64_t fences[2] = {word(fence), word(fence)};
        const std::uint64_t values[2] = {1, 2};
        const D3D12_RESIDENCY_PRIORITY priorities[4] = {};
        const auto same = [](const std::uint64_t* got, const std::uint64_t* want, int n) {
            return std::memcmp(got, want, n * sizeof(std::uint64_t)) == 0;
        };

        // 34 MakeResident, with a count whose high dword is not zero: the PC prototype takes a UINT.
        EXPECT(hr_of(raw(device, 34, 0xdeadbeef00000002ull, word(objects))) == S_OK);
        EXPECT(mock->calls[34] == 1 && static_cast<std::uint32_t>(mock->args[34][0]) == 2);
        EXPECT(same(mock->array[34], unwrapped, 2) && mock->array[34][2] == 0);
        // 31 CreateSharedHandle
        EXPECT(hr_of(raw(device, 31, word(fence), 0, GENERIC_ALL, 0, 0)) == S_OK);
        EXPECT(mock->calls[31] == 1 && mock->args[31][0] == real_fence);

        // 45/46: ID3D12Device1. A device without that interface has no PC entry there, so the slot is past its table:
        // the proxy stubs it with S_OK / 0 and never reaches the device (measured: refusing slots past the PC table
        // aborted the retail title - it calls slot 127 from 45 sites - so every one of them is stubbed and logged).
        const HRESULT r45 = hr_of(raw(device, 45, word(fences), word(values), 2, 0, 0));
        const HRESULT r46 = hr_of(raw(device, 46, 4, word(objects), word(priorities)));
        if (l.level >= 1) {
            EXPECT(r45 == S_OK && mock->calls[45] == 1 && mock->array[45][0] == real_fence &&
                   mock->array[45][1] == real_fence);
            EXPECT(r46 == S_OK && mock->calls[46] == 1 && same(mock->array[46], unwrapped, 4));
        } else {
            EXPECT(r45 == S_OK && r46 == S_OK && mock->calls[45] == 0 && mock->calls[46] == 0);
            void* device1 = nullptr;
            EXPECT(FAILED(hr_of(raw(device, 0, word(&kDevice1Iid), word(&device1)))) && !device1);
        }
        // 50: ID3D12Device3, with the fence argument unwrapped too. Past the table: stubbed like 45/46.
        const HRESULT r50 = hr_of(raw(device, 50, 0, 4, word(objects), word(fence), 1));
        if (l.level >= 3) {
            EXPECT(r50 == S_OK && mock->calls[50] == 1 && same(mock->array[50], unwrapped, 4) &&
                   mock->args[50][3] == real_fence);
        } else {
            EXPECT(r50 == S_OK && mock->calls[50] == 0);
        }
        // 75: ID3D12Device9::CreateCommandQueue1 - past the table on a device that predates Device9, so the stub answers
        // S_OK with the output still cleared and the device's own slot 75 never reached.
        void* queue1 = reinterpret_cast<void*>(1);  // cleared on every failure (bc59ef46's convention)
        const HRESULT r75 = hr_of(raw(device, 75, word(&kQueueDesc), 0, word(&kQueueIid), word(&queue1)));
        if (l.level >= 9) {
            EXPECT(r75 == S_OK && xs::is_queue_proxy(queue1) && mock->calls[75] == 1);
            EXPECT(release(queue1) == 0 && mock->last_child->refs == 0);
        } else {
            EXPECT(r75 == S_OK && !queue1 && mock->calls[75] == 0);
        }

        // 133: past every PC device vtable; the retail title calls it. Refused, not a jump through a missing entry.
        EXPECT(hr_of(raw(device, 133)) == E_NOTIMPL && mock->calls[127] == 0);

        // 133 with a handler (CreatePlacedResourceX): the arguments in the title's positions (va, desc, state,
        // clear, riid, out) reach the handler with the real device, the Xbox flag bits masked to the PC ones the host
        // takes (0x20004 -> 0x4, and 0x200 dropped) and the buffer's Xbox layout word made ROW_MAJOR.
        {
            static void* seen_real = nullptr;
            static void* seen_va = nullptr;
            static D3D12_RESOURCE_DESC seen_desc{};
            static GUID seen_iid{};
            static std::uint32_t seen_layout = 0;
            const xs::PlacedAtVaHandler handler = [](ID3D12Device* real_device, void* va,
                                                     const D3D12_RESOURCE_DESC& desc, std::uint32_t layout,
                                                     REFIID iid, void** out) -> HRESULT {
                seen_layout = layout;
                seen_real = real_device;
                seen_va = va;
                seen_desc = desc;
                seen_iid = iid;
                *out = reinterpret_cast<void*>(0x1330);
                return S_OK;
            };
            xs::set_placed_at_va_handler(handler);
            D3D12_RESOURCE_DESC xbox_desc{};
            xbox_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            xbox_desc.Width = 0x10000;
            xbox_desc.Height = xbox_desc.DepthOrArraySize = xbox_desc.MipLevels = 1;
            xbox_desc.SampleDesc.Count = 1;
            xbox_desc.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x1);  // the Xbox row-major word
            xbox_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x20204);
            const GUID resource_iid = __uuidof(ID3D12Resource);
            void* placed = nullptr;
            void* const va = reinterpret_cast<void*>(0x7f0000000000ull);
            const HRESULT r133 = hr_of(raw(device, 133, word(va), word(&xbox_desc), 0xFFFFFFFFull, 0,
                                           word(&resource_iid), word(&placed)));
            xs::set_placed_at_va_handler(nullptr);
            EXPECT(r133 == S_OK && placed == reinterpret_cast<void*>(0x1330));
            EXPECT(seen_real == xs::device_proxy_real_device(device) && seen_va == va);
            EXPECT(static_cast<std::uint32_t>(seen_desc.Flags) == 0x4);
            EXPECT(seen_desc.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR && seen_desc.Width == 0x10000);
            EXPECT(std::memcmp(&seen_iid, &resource_iid, sizeof(GUID)) == 0);
            EXPECT(seen_layout == 0xFFFFFFFFu);  // the title's third word, passed through as the barrier layout
            EXPECT(xs::device_slot_overridden(133));
        }

        // 133 with a pages hook (set_placed_at_va_pages_hook): a buffer's range [va, va + width) is handed to it
        // before the handler runs; a texture is not; a hook that fails still leaves the call to the handler.
        {
            static int order = 0;
            static int hook_at = 0;
            static int handler_at = 0;
            static const void* hook_va = nullptr;
            static std::size_t hook_bytes = 0;
            static bool hook_result = true;
            order = hook_at = handler_at = 0;
            const xs::PlacedAtVaPagesHook hook = [](const void* va, std::size_t bytes) -> bool {
                hook_at = ++order;
                hook_va = va;
                hook_bytes = bytes;
                return hook_result;
            };
            const xs::PlacedAtVaHandler handler = [](ID3D12Device*, void*, const D3D12_RESOURCE_DESC&, std::uint32_t,
                                                     REFIID, void** out) -> HRESULT {
                handler_at = ++order;
                *out = reinterpret_cast<void*>(0x1331);
                return S_OK;
            };
            xs::set_placed_at_va_handler(handler);
            xs::set_placed_at_va_pages_hook(hook);
            D3D12_RESOURCE_DESC xbox_desc{};
            xbox_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            xbox_desc.Width = 0xadf0000;
            xbox_desc.Height = xbox_desc.DepthOrArraySize = xbox_desc.MipLevels = 1;
            xbox_desc.SampleDesc.Count = 1;
            xbox_desc.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x1);
            const GUID resource_iid = __uuidof(ID3D12Resource);
            void* placed = nullptr;
            void* const va = reinterpret_cast<void*>(0x7f0000010000ull);
            auto place = [&]() {
                placed = nullptr;
                return hr_of(raw(device, 133, word(va), word(&xbox_desc), 0xFFFFFFFFull, 0, word(&resource_iid),
                                 word(&placed)));
            };
            const HRESULT buffer_hr = place();
            EXPECT(buffer_hr == S_OK && placed == reinterpret_cast<void*>(0x1331));
            EXPECT(hook_at == 1 && handler_at == 2 && hook_va == va && hook_bytes == 0xadf0000);

            hook_result = false;
            order = hook_at = handler_at = 0;
            const HRESULT refused_hr = place();
            EXPECT(refused_hr == S_OK && hook_at == 1 && handler_at == 2);
            hook_result = true;

            xbox_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            xbox_desc.Width = 64;
            xbox_desc.Height = 64;
            order = hook_at = handler_at = 0;
            const HRESULT texture_hr = place();
            EXPECT(texture_hr == S_OK && hook_at == 0 && handler_at == 1);

            xs::set_placed_at_va_pages_hook(nullptr);
            xs::set_placed_at_va_handler(nullptr);
        }

        // 127: the console's CreateCommittedResource with a barrier layout in the initial state's place (heap
        // properties, heap flags, desc, layout, clear value, protected session, castable-format count, castable
        // formats, riid, out). This is the call the title's display surfaces come from, so the out pointer must reach
        // the host's CreateCommittedResource: the proxy makes a PC committed resource from the console description
        // (layout word replaced, console flag bits masked, console alignment dropped) and maps the barrier layout to
        // the PC state it names.
        {
            D3D12_HEAP_PROPERTIES props{};
            props.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC xbox_desc{};
            xbox_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            xbox_desc.Width = 1920;
            xbox_desc.Height = 1080;
            xbox_desc.DepthOrArraySize = 1;
            xbox_desc.MipLevels = 1;
            xbox_desc.SampleDesc.Count = 1;
            xbox_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            xbox_desc.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x101);  // the Xbox layout word
            xbox_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x20005);  // console bits + RT | UAV
            xbox_desc.Alignment = 0x1234;                                 // not a PC placement alignment
            D3D12_CLEAR_VALUE clear{};
            clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            const GUID resource_iid = __uuidof(ID3D12Resource);
            void* out = nullptr;
            const HRESULT r127 = hr_of(raw(device, 127, word(&props), 8 /* D3D12_HEAP_FLAG_ALLOW_DISPLAY */,
                                           word(&xbox_desc), 3 /* D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS */,
                                           word(&clear), 0, 0, 0, word(&resource_iid), word(&out)));
            EXPECT(r127 == S_OK && out == reinterpret_cast<void*>(0x1270));
            EXPECT(mock->calls[27] == 1 && mock->committed_out_written);
            EXPECT(mock->committed_heap_flags == D3D12_HEAP_FLAG_ALLOW_DISPLAY);
            EXPECT(mock->committed_state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            EXPECT(mock->committed_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D);
            EXPECT(mock->committed_desc.Width == 1920 && mock->committed_desc.Height == 1080);
            EXPECT(mock->committed_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM);
            EXPECT(mock->committed_desc.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN);
            EXPECT(mock->committed_desc.Alignment == 0);
            EXPECT(static_cast<std::uint32_t>(mock->committed_desc.Flags) == 0x05);
            EXPECT(mock->committed_props.Type == D3D12_HEAP_TYPE_DEFAULT);
            EXPECT(xs::device_slot_overridden(127));

            // The title's display-surface window (docs/range_assert.md): a display surface created here asks the
            // installed title allocator for its byte size with the hook's routing word, while the same creation without
            // ALLOW_DISPLAY does not. The recorder stands in for the title's exported XMem allocation hook chain.
            static unsigned surface_calls = 0;
            static std::uint64_t surface_bytes = 0;
            static std::uint32_t surface_attributes = 0;
            static auto record_surface = [](std::uint64_t bytes, std::uint32_t attributes) -> void* {
                ++surface_calls;
                surface_bytes = bytes;
                surface_attributes = attributes;
                return nullptr;
            };
            const xs::TitleSurfaceAllocator previous_allocator = xs::title_surface_allocator();
            xs::set_title_surface_allocator(record_surface);
            surface_calls = 0;
            void* surface_out = nullptr;
            const HRESULT display_hr =
                hr_of(raw(device, 127, word(&props), 8 /* D3D12_HEAP_FLAG_ALLOW_DISPLAY */, word(&xbox_desc),
                          3 /* D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS */, word(&clear), 0, 0, 0,
                          word(&resource_iid), word(&surface_out)));
            EXPECT(display_hr == S_OK);
            // The measured surface shape is single-sample, single-mip, 1-plane RGBA8: width x height x 4 bytes, with the
            // routing word (bucket 0x80, reserve-and-carve bits 0x8000).
            EXPECT(surface_calls == 1);
            EXPECT(surface_bytes == 1920ull * 1080ull * 4ull);
            EXPECT(surface_attributes == 0x00808000u);
            void* plain_out = nullptr;
            const HRESULT plain_hr = hr_of(raw(device, 127, word(&props), 0, word(&xbox_desc),
                                               3 /* D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS */, word(&clear), 0, 0, 0,
                                               word(&resource_iid), word(&plain_out)));
            EXPECT(plain_hr == S_OK);
            EXPECT(surface_calls == 1);
            xs::set_title_surface_allocator(previous_allocator);

            // The window arithmetic the slot feeds (docs/range_assert.md sections 2.2 and 2.5), modelled from the
            // measured constants: each carve is rounded up to 64 KiB, and game+0x247af25 passes only when what is
            // left of the window is <= 2 MiB - 1. The pre-fix runtime's untouched 64 MiB window is exactly the value
            // the check saw then (0x4000000), and two measured 3840x2160 RGBA8 surfaces leave 0xA0000.
            {
                const auto round_64k = [](std::uint64_t bytes) { return (bytes + 0xFFFFull) & ~0xFFFFull; };
                const std::uint64_t window = 0x4000000ull;  // 64 MiB: this host reads the 64 MiB branch
                const std::uint64_t carve = round_64k(3840ull * 2160ull * 4ull);
                EXPECT(carve == 0x1FB0000ull);
                EXPECT(window - 2 * carve == 0xA0000ull);
                EXPECT(window - 2 * carve < 0x200000ull);  // the fixed runtime passes
                EXPECT(window - 0 >= 0x200000ull);         // the pre-fix runtime's value fired
                EXPECT(0x1FFFFFull < 0x200000ull);         // 2 MiB - 1 passes the title's jbe
                EXPECT(!(0x200000ull < 0x200000ull));      // 2 MiB does not
            }
        }

        Mock* const queue_mock = reinterpret_cast<Mock*>(real_queue);
        Mock* const fence_mock = reinterpret_cast<Mock*>(real_fence);
        EXPECT(release(queue) == 0 && release(fence) == 0);
        EXPECT(queue_mock->refs == 0 && fence_mock->refs == 0);
        EXPECT(release(device) == 0 && mock->refs == 0);
    }
    std::printf("%s: devices up to ID3D12Device, Device1, Device3, Device9: 44/47/51/76 slots, 45/46/50/75 stubbed "
                "past the table without reaching the device, residency arrays and fences unwrapped otherwise\n", g_label);
    return 0;
}

// ---- 2. Fences with and without ID3D12Fence1 ----

int check_fence1() {
    for (const bool fence1 : {false, true}) {
        Mock* mock = nullptr;
        void* device = make_device(mock, 9, fence1);
        EXPECT(device);
        const unsigned long device_refs = xs::device_proxy_ref_count(device);
        void* fence = make_fence(device);
        EXPECT(xs::is_fence_proxy(fence) && xs::fence_proxy_slot_count(fence) == (fence1 ? 12u : 11u));
        Mock* const fence_mock = mock->last_child;
        void* as_fence1 = nullptr;
        const HRESULT qi = hr_of(raw(fence, 0, word(&kFence1Iid), word(&as_fence1)));
        if (fence1) {
            EXPECT(qi == S_OK && as_fence1 == fence && release(as_fence1) == 1);
        } else {
            EXPECT(qi == E_NOINTERFACE && !as_fence1);
        }
        EXPECT(raw(fence, 8) == kCompletedValue);  // GetCompletedValue forwarded
        const HRESULT slot11 = hr_of(raw(fence, 11));
        if (fence1) {
            EXPECT(slot11 == S_OK && fence_mock->calls[11] == 1);
        } else {
            EXPECT(slot11 == E_NOTIMPL && fence_mock->calls[11] == 0);  // not forwarded past an 11-slot vtable
        }
        // Console fence slot 12 (docs/fence_slot12.md): the address of the fence's completed-value cell, answered by
        // the proxy (not forwarded), fixed for the fence, and watched only for a target the value has not reached.
        g_watch_calls = 0;
        g_watch_target = 0;
        g_watched_cell = nullptr;
        const std::uint64_t cell_address = raw(fence, 12, kCompletedValue);
        EXPECT(cell_address != 0 && fence_mock->calls[12] == 0);
        auto* const cell = reinterpret_cast<std::atomic<std::uint64_t>*>(cell_address);
        EXPECT(cell->load() == kCompletedValue && g_watch_calls == 0);  // reached: no watch asked for
        EXPECT(raw(fence, 12, kCompletedValue + 2) == cell_address && fence_mock->calls[12] == 0);
        EXPECT(g_watch_calls == 1 && g_watch_target == kCompletedValue + 2 && g_watched_cell == cell);
        // A Signal through a queue proxy raises the mock fence's value and the proxy publishes it into the same cell.
        void* queue = make_queue(device);
        Mock* const real_queue = mock->last_child;
        EXPECT(queue && real_queue && real_queue->kind == Kind::queue);
        EXPECT(hr_of(raw(queue, 16, word(fence), kCompletedValue + 5, 0, 0)) == S_OK);
        EXPECT(real_queue->calls[14] == 1 && real_queue->args[14][0] == word(fence_mock) &&
               real_queue->args[14][1] == kCompletedValue + 5);
        EXPECT(fence_mock->fence_value == kCompletedValue + 5 && cell->load() == kCompletedValue + 5);
        EXPECT(raw(fence, 12, kCompletedValue + 1) == cell_address && cell->load() == kCompletedValue + 5 &&
               g_watch_calls == 1);  // already reached: no second watch
        EXPECT(release(queue) == 0 && real_queue->refs == 0);
        EXPECT(release(fence) == 0 && fence_mock->refs == 0);

        // Asked for as ID3D12Fence1: made as ID3D12Fence, then refused when the fence lacks Fence1, and released.
        void* created1 = nullptr;
        const HRESULT made1 = hr_of(raw(device, 36, 0, 0, word(&kFence1Iid), word(&created1)));
        if (fence1) {
            EXPECT(made1 == S_OK && xs::is_fence_proxy(created1) && release(created1) == 0);
        } else {
            EXPECT(made1 == E_NOINTERFACE && !created1);
        }
        EXPECT(mock->last_child->refs == 0);
        EXPECT(xs::device_proxy_ref_count(device) == device_refs);
        EXPECT(release(device) == 0 && mock->refs == 0);
    }
    std::printf("%s: fences without and with ID3D12Fence1: 11/12 slots, QueryInterface, slot 11 and creation as "
                "Fence1 refused or forwarded to match\n", g_label);
    return 0;
}

// ---- 3. Creation failures and refused interfaces ----

int check_create_failures() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    const unsigned long device_refs = xs::device_proxy_ref_count(device);
    void* out = reinterpret_cast<void*>(1);

    mock->create_null = true;  // S_OK with no object
    mock->create_result = S_OK;
    EXPECT(hr_of(raw(device, 8, word(&kQueueDesc), word(&kQueueIid), word(&out))) == E_FAIL && !out);
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 36, 0, 0, word(&kFenceIid), word(&out))) == E_FAIL && !out);
    mock->create_null = false;
    mock->create_result = E_OUTOFMEMORY;  // the real failure, passed on
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 8, word(&kQueueDesc), word(&kQueueIid), word(&out))) == E_OUTOFMEMORY && !out);
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 75, word(&kQueueDesc), 0, word(&kQueueIid), word(&out))) == E_OUTOFMEMORY && !out);
    mock->create_result = S_OK;

    const unsigned calls8 = mock->calls[8];
    out = reinterpret_cast<void*>(1);  // a null IID: E_POINTER, the output cleared, nothing created
    EXPECT(hr_of(raw(device, 8, word(&kQueueDesc), 0, word(&out))) == E_POINTER && !out && mock->calls[8] == calls8);
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 36, 0, 0, 0, word(&out))) == E_POINTER && !out);
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 75, word(&kQueueDesc), 0, 0, word(&out))) == E_POINTER && !out);
    // No output: the device's own answer (S_FALSE here), as the PC runtime gives it.
    EXPECT(hr_of(raw(device, 8, word(&kQueueDesc), word(&kQueueIid), 0)) == S_FALSE && mock->calls[8] == calls8 + 1);
    EXPECT(hr_of(raw(device, 36, 0, 0, word(&kFenceIid), 0)) == S_FALSE);

    // An interface the real queue implements but the proxy does not model: created, refused, released.
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 8, word(&kQueueDesc), word(&kExtraIid), word(&out))) == E_NOINTERFACE && !out);
    EXPECT(mock->last_child->refs == 0);
    void* queue = make_queue(device);
    EXPECT(queue);
    Mock* const queue_mock = mock->last_child;
    void* extra = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(queue, 0, word(&kExtraIid), word(&extra))) == E_NOINTERFACE && !extra);
    EXPECT(xs::command_object_proxy_ref_count(queue) == 1 && queue_mock->refs == 1);
    EXPECT(release(queue) == 0 && queue_mock->refs == 0);
    EXPECT(xs::device_proxy_ref_count(device) == device_refs);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: creation with no object (E_FAIL), a failing device, null IID and output, and an interface the "
                "proxy does not model leave no reference\n", g_label);
    return 0;
}

// ---- 4. OpenSharedHandle ----

int check_open_shared() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, false);
    EXPECT(device);
    const unsigned long device_refs = xs::device_proxy_ref_count(device);

    for (const IID* iid : {&kFenceIid, &kUnknownIid}) {  // a fence, whatever it is asked for as, comes back wrapped
        void* opened = nullptr;
        EXPECT(hr_of(raw(device, 32, kFenceHandle, word(iid), word(&opened))) == S_OK);
        Mock* const fence_mock = mock->last_child;
        EXPECT(xs::is_fence_proxy(opened) && word(xs::fence_proxy_real_fence(opened)) == word(fence_mock));
        EXPECT(xs::command_object_proxy_ref_count(opened) == 1 && fence_mock->refs == 1);
        EXPECT(xs::device_proxy_ref_count(device) == device_refs + 1);
        EXPECT(release(opened) == 0 && fence_mock->refs == 0);
    }
    void* out = reinterpret_cast<void*>(1);  // as Fence1, which this fence lacks: the real open fails, passed on
    EXPECT(hr_of(raw(device, 32, kFenceHandle, word(&kFence1Iid), word(&out))) == E_NOINTERFACE && !out);
    EXPECT(mock->last_child->refs == 0);

    for (const IID* iid : {&kResourceIid, &kUnknownIid}) {  // not a fence: the real object, unchanged
        void* opened = nullptr;
        EXPECT(hr_of(raw(device, 32, kResourceHandle, word(iid), word(&opened))) == S_OK);
        Mock* const resource_mock = mock->last_child;
        EXPECT(opened == resource_mock && !xs::is_fence_proxy(opened) && !xs::is_queue_proxy(opened));
        EXPECT(resource_mock->refs == 1 && release(opened) == 0);
    }
    EXPECT(xs::device_proxy_ref_count(device) == device_refs);

    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 32, 0xbad, word(&kFenceIid), word(&out))) == E_INVALIDARG && !out);
    EXPECT(hr_of(raw(device, 32, kFenceHandle, word(&kFenceIid), 0)) == S_FALSE);  // the device's own answer
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 32, kFenceHandle, 0, word(&out))) == E_POINTER && !out);
    EXPECT(xs::device_proxy_ref_count(device) == device_refs);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: OpenSharedHandle wraps a fence (as ID3D12Fence and IUnknown), passes a resource, a failed open, "
                "an invalid handle and a null output through, with balanced references\n", g_label);
    return 0;
}

// ---- 5. Allocation failures in the proxy ----

int check_alloc_failures() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    const unsigned long device_refs = xs::device_proxy_ref_count(device);
    void* queue = make_queue(device);
    void* fence = make_fence(device);
    EXPECT(queue && fence);
    const std::uint64_t objects[2] = {word(queue), word(fence)};

    const auto failing = [&](auto call) {  // one allocation refused, the result of the call
        const int before = g_failed_news;
        g_fail_news = 1;
        const HRESULT hr = call();
        g_fail_news = 0;
        return g_failed_news == before + 1 ? hr : S_OK;
    };
    void* out = reinterpret_cast<void*>(1);
    EXPECT(failing([&] { return hr_of(raw(device, 36, 0, 0, word(&kFenceIid), word(&out))); }) == E_OUTOFMEMORY);
    EXPECT(!out && mock->last_child->refs == 0);  // the real fence was made and released
    out = reinterpret_cast<void*>(1);
    EXPECT(failing([&] { return hr_of(raw(device, 8, word(&kQueueDesc), word(&kQueueIid), word(&out))); }) ==
           E_OUTOFMEMORY);
    EXPECT(!out && mock->last_child->refs == 0);
    out = reinterpret_cast<void*>(1);
    EXPECT(failing([&] { return hr_of(raw(device, 32, kFenceHandle, word(&kFenceIid), word(&out))); }) ==
           E_OUTOFMEMORY);
    EXPECT(!out && mock->last_child->refs == 0);
    // Up to 16 entries the unwrapped copy is inline: an armed failure is not even reached, and the call goes on.
    g_fail_news = 1;
    const HRESULT inline_result = hr_of(raw(device, 34, 2, word(objects)));
    const bool untouched = g_fail_news == 1;
    g_fail_news = 0;
    EXPECT(inline_result == S_OK && untouched && mock->array[34][0] == word(xs::queue_proxy_real_queue(queue)));

    // A longer array is allocated; refused, the call returns E_OUTOFMEMORY and reaches no device.
    constexpr unsigned kLong = 32;
    std::uint64_t many[kLong], many_fences[kLong], values[kLong];
    D3D12_RESIDENCY_PRIORITY priorities[kLong] = {};
    for (unsigned i = 0; i < kLong; ++i) {
        many[i] = (i % 2 != 0) ? word(fence) : word(queue);
        many_fences[i] = word(fence);
        values[i] = 1;
    }
    const unsigned calls34 = mock->calls[34], calls45 = mock->calls[45], calls46 = mock->calls[46];
    const unsigned calls50 = mock->calls[50];
    EXPECT(failing([&] { return hr_of(raw(device, 34, kLong, word(many))); }) == E_OUTOFMEMORY);
    EXPECT(failing([&] { return hr_of(raw(device, 45, word(many_fences), word(values), kLong, 0, 0)); }) ==
           E_OUTOFMEMORY);
    EXPECT(failing([&] { return hr_of(raw(device, 46, kLong, word(many), word(priorities))); }) == E_OUTOFMEMORY);
    EXPECT(failing([&] { return hr_of(raw(device, 50, 0, kLong, word(many), word(fence), 1)); }) == E_OUTOFMEMORY);
    EXPECT(mock->calls[34] == calls34 && mock->calls[45] == calls45 && mock->calls[46] == calls46 &&
           mock->calls[50] == calls50);  // nothing reached the device
    // And without the failure the long array arrives unwrapped.
    EXPECT(hr_of(raw(device, 34, kLong, word(many))) == S_OK && mock->calls[34] == calls34 + 1);
    EXPECT(mock->array[34][0] == word(xs::queue_proxy_real_queue(queue)) &&
           mock->array[34][1] == word(xs::fence_proxy_real_fence(fence)));

    // The device proxy itself: on failure the caller keeps its reference.
    Mock* other = new_mock(Kind::device);
    other->refs = 1;
    void* second = reinterpret_cast<void*>(1);
    EXPECT(failing([&] {
               return xs::create_device_proxy(reinterpret_cast<ID3D12Device*>(other), __uuidof(ID3D12Device),
                                              &second);
           }) == E_OUTOFMEMORY);
    EXPECT(!second && other->refs == 1);

    Mock* const queue_mock = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue));
    Mock* const fence_mock = reinterpret_cast<Mock*>(xs::fence_proxy_real_fence(fence));
    EXPECT(release(queue) == 0 && release(fence) == 0 && queue_mock->refs == 0 && fence_mock->refs == 0);
    EXPECT(xs::device_proxy_ref_count(device) == device_refs);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: a refused allocation in queue, fence, shared-fence and device proxy creation and in the four "
                "unwrapped arrays returns E_OUTOFMEMORY, reaches no device and leaks nothing\n", g_label);
    return 0;
}

// ---- 6. Command allocators (device slot 9) ----

int check_allocators() {
    const IID allocator_iid = __uuidof(ID3D12CommandAllocator);
    // An allocator that answers Palmetto's tag (its lists resolve a proxy) comes back wrapped.
    {
        Mock* mock = nullptr;
        void* device = make_device(mock, 9, true, true);
        EXPECT(device);
        const unsigned long device_refs = xs::device_proxy_ref_count(device);
        void* allocator = nullptr;
        EXPECT(hr_of(raw(device, 9, D3D12_COMMAND_LIST_TYPE_DIRECT, word(&allocator_iid), word(&allocator))) == S_OK);
        Mock* const real = mock->last_child;
        EXPECT(xs::is_allocator_proxy(allocator) && word(xs::allocator_proxy_real_allocator(allocator)) == word(real));
        EXPECT(mock->args[9][0] == D3D12_COMMAND_LIST_TYPE_DIRECT && real->refs == 1);
        EXPECT(xs::allocator_proxy_slot_count(allocator) == 9 && xs::device_proxy_ref_count(device) == device_refs + 1);
        // Reset (slot 8) reaches the real allocator. Console slot 9 is the callback registration: the pair is recorded
        // on the proxy and the call answers S_OK without reaching the real allocator's vtable; a later pair replaces it,
        // zeros clear it, the record is per proxy, and every slot past 9 up to the table end is refused.
        EXPECT(hr_of(raw(allocator, 8)) == S_OK && real->calls[8] == 1);
        EXPECT(xs::allocator_proxy_callback(allocator).callback == 0 &&
               xs::allocator_proxy_callback(allocator).context == 0);
        EXPECT(xs::allocator_proxy_callback(nullptr).callback == 0 &&
               xs::allocator_proxy_callback(device).context == 0);
        constexpr std::uintptr_t cb = 0x12345678u;
        constexpr std::uintptr_t ctx = 0x9abcdef0u;
        const std::uint64_t slot9_hits = xs::allocator_slot_hits(9);
        EXPECT(hr_of(raw(allocator, 9, cb, ctx, 0xdead, 0xbeef)) == S_OK && real->calls[9] == 0);
        EXPECT(xs::allocator_slot_hits(9) == slot9_hits + 1 && xs::allocator_slot_first_call_seen(9));
        xs::AllocatorCallback record = xs::allocator_proxy_callback(allocator);
        EXPECT(record.callback == cb && record.context == ctx);
        EXPECT(hr_of(raw(allocator, 9, 0x2222, 0x3333)) == S_OK && real->calls[9] == 0);
        record = xs::allocator_proxy_callback(allocator);
        EXPECT(record.callback == 0x2222 && record.context == 0x3333);
        EXPECT(hr_of(raw(allocator, 9, 0, 0)) == S_OK && real->calls[9] == 0);
        record = xs::allocator_proxy_callback(allocator);
        EXPECT(record.callback == 0 && record.context == 0);
        // A second allocator proxy keeps its own record; the first one is unchanged, and releasing a proxy drops its
        // record (the next proxy created starts empty rather than inheriting static state).
        void* other_allocator = nullptr;
        EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&other_allocator))) == S_OK);
        Mock* const other_real = mock->last_child;
        EXPECT(xs::is_allocator_proxy(other_allocator) && other_real && other_real->calls[9] == 0);
        EXPECT(xs::allocator_proxy_callback(other_allocator).callback == 0);
        EXPECT(hr_of(raw(other_allocator, 9, 0x4444, 0x5555)) == S_OK);
        record = xs::allocator_proxy_callback(other_allocator);
        EXPECT(record.callback == 0x4444 && record.context == 0x5555);
        EXPECT(xs::allocator_proxy_callback(allocator).callback == 0);
        EXPECT(release(other_allocator) == 0 && other_real->refs == 0);
        void* reused = nullptr;
        EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&reused))) == S_OK);
        EXPECT(xs::is_allocator_proxy(reused) && xs::allocator_proxy_callback(reused).callback == 0 &&
               xs::allocator_proxy_callback(reused).context == 0);
        EXPECT(release(reused) == 0 && mock->last_child->refs == 0);
        for (unsigned slot = 10; slot < xs::allocator_proxy_table_size(); ++slot)
            EXPECT(hr_of(raw(allocator, slot)) == E_NOTIMPL && real->calls[slot] == 0);
        // The tag and the real object stay hidden: QueryInterface answers only the modelled interfaces.
        void* found = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(allocator, 0, word(&xs::kPalmettoCommandAllocatorTag), word(&found))) == E_NOINTERFACE &&
               !found);
        EXPECT(hr_of(raw(allocator, 0, word(&allocator_iid), word(&found))) == S_OK && found == allocator);
        EXPECT(release(found) == 1);
        // An allocator is a pageable: MakeResident gives the real runtime the real allocator.
        const std::uint64_t objects[1] = {word(allocator)};
        EXPECT(hr_of(raw(device, 34, 1, word(objects))) == S_OK && mock->array[34][0] == word(real));
        EXPECT(release(allocator) == 0 && real->refs == 0);
        EXPECT(xs::device_proxy_ref_count(device) == device_refs);
        // Null IID, null output, a failing or empty creation, an IID the allocator lacks.
        void* out = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device, 9, 0, 0, word(&out))) == E_POINTER && !out);
        EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), 0)) == S_FALSE);  // the device's own answer
        mock->create_result = E_OUTOFMEMORY;
        out = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&out))) == E_OUTOFMEMORY && !out);
        mock->create_result = S_OK;
        mock->create_null = true;
        out = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&out))) == E_FAIL && !out);
        mock->create_null = false;
        const IID fence_iid = __uuidof(ID3D12Fence);
        out = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device, 9, 0, word(&fence_iid), word(&out))) == E_NOINTERFACE && !out);
        EXPECT(mock->last_child->refs == 0);
        // A refused allocation for the proxy: E_OUTOFMEMORY, and the real allocator is released.
        g_fail_news = 1;
        out = reinterpret_cast<void*>(1);
        const HRESULT starved = hr_of(raw(device, 9, 0, word(&allocator_iid), word(&out)));
        g_fail_news = 0;
        EXPECT(starved == E_OUTOFMEMORY && !out && mock->last_child->refs == 0);
        EXPECT(xs::device_proxy_ref_count(device) == device_refs);
        EXPECT(release(device) == 0 && mock->refs == 0);
    }
    // An allocator without the tag (a PC runtime allocator, whose lists could not resolve a proxy) comes back as it
    // is, as the interface asked for.
    {
        Mock* mock = nullptr;
        void* device = make_device(mock, 9, true, false);
        EXPECT(device);
        void* allocator = nullptr;
        EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&allocator))) == S_OK);
        EXPECT(allocator == mock->last_child && !xs::is_allocator_proxy(allocator) && mock->last_child->refs == 1);
        Mock* const real = mock->last_child;
        EXPECT(release(allocator) == 0 && real->refs == 0);
        const IID fence_iid = __uuidof(ID3D12Fence);
        void* out = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device, 9, 0, word(&fence_iid), word(&out))) == E_NOINTERFACE && !out);
        EXPECT(mock->last_child->refs == 0);
        EXPECT(release(device) == 0 && mock->refs == 0);
    }
    std::printf("%s: allocators answering Palmetto's tag are wrapped (Reset forwarded, console slot 9's callback "
                "recorded and answered S_OK, tail refused, residency unwrapped, references balanced); others come back "
                "unwrapped; failures leave no reference\n", g_label);
    return 0;
}

// ---- 7a. The present-driven frame events: what the proxy forwards (docs/peer_port_first_ui.md, the implemented port) ----
//
// With the switch on, the registered frame-event backend answers slots 78/79/80: SetFrameIntervalX and the origin
// ScheduleFrameEventX are forwarded, the origin poll (event 0, flag 2) hands back the pacer's last token with S_FALSE
// when no new origin is due, and the event-2 wait is the pacer's blocking flip wait. With the switch off (no backend
// registered) every answer is the proxy's own, unchanged. The backend is a fake: this executable links no runtime.
namespace {
struct FakeFrameEvents {
    unsigned set_interval = 0, schedule = 0, waits = 0, flips = 0;
    std::uint32_t interval_us = 0, latency = 0, flags = 0, event = 0, offset_us = 0;
    bool wait_succeeds = false;
    std::uint64_t wait_token = 0;
};
FakeFrameEvents* g_fake_frame_events = nullptr;

BOOL WINAPI fake_frame_events_enabled() { return g_fake_frame_events ? TRUE : FALSE; }
HRESULT WINAPI fake_set_frame_interval(std::uint32_t interval_us, std::uint32_t latency, std::uint32_t flags) {
    if (!g_fake_frame_events) return E_UNEXPECTED;
    g_fake_frame_events->set_interval += 1;
    g_fake_frame_events->interval_us = interval_us;
    g_fake_frame_events->latency = latency;
    g_fake_frame_events->flags = flags;
    return S_OK;
}
HRESULT WINAPI fake_schedule_frame_event(std::uint32_t event, std::uint32_t offset_us, std::uint32_t flags) {
    if (!g_fake_frame_events) return E_UNEXPECTED;
    g_fake_frame_events->schedule += 1;
    g_fake_frame_events->event = event;
    g_fake_frame_events->offset_us = offset_us;
    g_fake_frame_events->flags = flags;
    return S_OK;
}
HRESULT WINAPI fake_wait_frame_event(std::uint32_t, std::uint32_t, std::uint32_t, std::uint64_t* token) {
    if (!g_fake_frame_events) return E_UNEXPECTED;
    g_fake_frame_events->waits += 1;
    if (!g_fake_frame_events->wait_succeeds) return S_FALSE;  // no new origin due
    *token = g_fake_frame_events->wait_token;
    return S_OK;
}
HRESULT WINAPI fake_wait_flip(std::uint32_t) {
    if (!g_fake_frame_events) return E_UNEXPECTED;
    g_fake_frame_events->flips += 1;
    return S_OK;
}
std::uint64_t WINAPI fake_last_token() { return 42; }
}  // namespace

int check_frame_event_forwarding() {
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
    Mock* mock = nullptr;
    void* device = make_device(mock, 11, true);
    EXPECT(device != nullptr);
    FakeFrameEvents fake;
    const rt::d3d12::XboxFrameEventBackend events{&fake_frame_events_enabled,     &fake_set_frame_interval,
                                                  &fake_schedule_frame_event,     &fake_wait_frame_event,
                                                  &fake_wait_flip,                &fake_last_token};
    rt::d3d12::XboxPresentBackend record{};
    record.size = sizeof(record);
    record.version = rt::d3d12::xbox_present_backend_version;
    record.frame_events = &events;
    g_fake_frame_events = &fake;
    rt::d3d12::xbox_present_hook::registered = &record;
    EXPECT(rt::d3d12::xbox_frame_events_enabled());

    // 78: forwarded to the pacer with the title's measured arguments (period 33333 us, latency 3), not to PC slot 78.
    EXPECT(hr_of(raw(device, 78, 0, 33333, 3, 0)) == S_OK);
    EXPECT(fake.set_interval == 1 && fake.interval_us == 33333 && fake.latency == 3 && mock->calls[78] == 0);
    // 79: the origin event is forwarded; the later frame point (event 2) is kept by the proxy with S_OK.
    EXPECT(hr_of(raw(device, 79, 0, 0, 0, 0)) == S_OK);
    EXPECT(fake.schedule == 1 && fake.event == 0 && mock->calls[79] == 0);
    EXPECT(hr_of(raw(device, 79, 2, 0, 0, 0)) == S_OK);
    EXPECT(fake.schedule == 1);
    // 80 event 0, flag 2: no new origin is due, so the poll returns S_FALSE with the pacer's last token (42) written.
    std::uint64_t token = 0;
    EXPECT(hr_of(raw(device, 80, 0, 0, 0, 2, word(&token))) == S_FALSE && token == 42 && fake.waits == 1);
    // 80 event 0, no flag: a new origin is due, the pacer's token and S_OK are passed through.
    fake.wait_succeeds = true;
    fake.wait_token = 7;
    token = 0;
    EXPECT(hr_of(raw(device, 80, 0, 0xffffffffu, 0, 0, word(&token))) == S_OK && token == 7 && fake.waits == 2);
    // 80 event 2 (INFINITE): the blocking flip wait, no token written.
    token = 9;
    EXPECT(hr_of(raw(device, 80, 2, 0xffffffffu, 0, 0, word(&token))) == S_OK && fake.flips == 1 && token == 9);
    EXPECT(mock->calls[80] == 0);

    // Switch off (nothing registered): the proxy's own counter answers again, exactly as before.
    rt::d3d12::xbox_present_hook::registered = nullptr;
    EXPECT(!rt::d3d12::xbox_frame_events_enabled());
    token = 0;
    EXPECT(hr_of(raw(device, 80, 0, 0, 0, 0, word(&token))) == S_OK && token != 0);
    EXPECT(hr_of(raw(device, 78, 0, 16667, 1, 0)) == S_OK);
    EXPECT(hr_of(raw(device, 79, 0, 0, 0x1234, 0)) == S_OK);
    EXPECT(mock->calls[78] == 0 && mock->calls[79] == 0 && mock->calls[80] == 0);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: with the present-driven clock on the proxy forwards slots 78/79/80 to the pacer (interval, origin "
                "event, the flag-2 origin poll with the last token, the event-2 flip wait) and never reaches the PC "
                "slots; with it off the old token counter answers, unchanged\n",
                g_label);
    return 0;
#else
    std::printf("%s: ID3D12Device11 is not in this SDK; the frame-event forwarding case is not built\n", g_label);
    return 0;
#endif
}

// ---- 7. Xbox device slots from the public HLE (48, 73, 78, 79, 80) ----
//
// A device that answers ID3D12Device11 (80 PC slots) is the Vulkan layer's shape: slots 78/79 are inside its table
// (CreateReservedResource2/CreateSampler2 there) and 80 is just past it. None of the five may reach the mock's own
// slot: the mock's call counter proves the route, and the returned token/queue/list proves the handler ran.
int check_xbox_slots() {
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
    Mock* mock = nullptr;
    void* device = make_device(mock, 11, true);
    EXPECT(device && xs::device_proxy_slot_count(device) == 80);
    xs::reset_device_proxy_stats();

    // 78 SetFrameIntervalX(output, interval_us, latency, flags): S_OK, not forwarded to PC slot 78.
    EXPECT(hr_of(raw(device, 78, 0, 16667, 1, 0)) == S_OK);
    EXPECT(mock->calls[78] == 0 && xs::device_slot_hits(78) == 1);
    // 79 ScheduleFrameEventX(event, offset_us, pEvent, flags): S_OK, not forwarded to PC slot 79.
    EXPECT(hr_of(raw(device, 79, 0, 0, 0x1234, 0)) == S_OK);
    EXPECT(mock->calls[79] == 0 && xs::device_slot_hits(79) == 1);

    // 80 WaitFrameEventX(event, timeout_ms, pEvent, flags, &token): a fresh non-zero token each time; a null token
    // pointer is E_POINTER; the call never blocks (this test would hang otherwise).
    std::uint64_t token = 0, token2 = 0;
    EXPECT(hr_of(raw(device, 80, 0, 0xffffffffu, 0, 0, word(&token))) == S_OK && token != 0);
    EXPECT(hr_of(raw(device, 80, 0, 0, 0, 0, word(&token2))) == S_OK && token2 != 0 && token2 != token);
    EXPECT(hr_of(raw(device, 80, 0, 0, 0, 0, 0)) == E_POINTER);
    EXPECT(mock->calls[80] == 0 && xs::device_slot_hits(80) == 3);

    // 48 CreateCommandQueueX(desc, riid, out): the queue proxy, created through the PC slot 8 path. The console
    // description starts with a PC D3D12_COMMAND_QUEUE_DESC and carries console Flags bits the host refuses, so the
    // proxy copies the 16-byte prefix and masks Flags to the one PC bit (measured: the unmasked bits are what
    // "CreateCommandQueue refused (type, priority, flags or node mask)" reported). The real slot 8 therefore sees the
    // proxy's copy - the same type, priority and node mask, Flags 0x201 -> 0x1 - not the title's pointer.
    D3D12_COMMAND_QUEUE_DESC xbox_queue_desc{};
    xbox_queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    xbox_queue_desc.Priority = 0x11;
    xbox_queue_desc.Flags = static_cast<D3D12_COMMAND_QUEUE_FLAGS>(0x201);
    xbox_queue_desc.NodeMask = 3;
    void* queue = nullptr;
    EXPECT(hr_of(raw(device, 48, word(&xbox_queue_desc), word(&kQueueIid), word(&queue))) == S_OK);
    EXPECT(xs::is_queue_proxy(queue) && mock->calls[48] == 0 && mock->calls[8] == 1);
    EXPECT(mock->queue_desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT && mock->queue_desc.Priority == 0x11);
    EXPECT(static_cast<std::uint32_t>(mock->queue_desc.Flags) ==
           static_cast<std::uint32_t>(D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT));
    EXPECT(mock->queue_desc.NodeMask == 3);

    // 73 CreateCommandListX(desc, allocator, initialState, riid, out): the type is desc's first word, the allocator
    // proxy is unwrapped, the node mask is 0 and the PC six-argument CreateCommandList (slot 12) is reached.
    const IID allocator_iid = __uuidof(ID3D12CommandAllocator);
    void* allocator = nullptr;
    EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&allocator))) == S_OK);
    EXPECT(xs::is_allocator_proxy(allocator));
    Mock* const real_allocator = reinterpret_cast<Mock*>(xs::allocator_proxy_real_allocator(allocator));
    struct XboxCommandListDesc {
        std::uint32_t type;
        std::uint32_t node_mask;
        std::uint64_t reserved[2];
    } desc{};
    desc.type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    desc.node_mask = 7;  // ignored by the console call, as the HLE does
    const IID list_iid = __uuidof(ID3D12GraphicsCommandList);
    void* list = nullptr;
    EXPECT(hr_of(raw(device, 73, word(&desc), word(allocator), 0, word(&list_iid), word(&list))) == S_OK);
    EXPECT(xs::is_command_list_proxy(list));
    EXPECT(xs::command_list_proxy_real_list(list) &&
           reinterpret_cast<Mock*>(xs::command_list_proxy_real_list(list))->kind == Kind::command_list);
    EXPECT(mock->calls[73] == 0 && mock->calls[12] == 1 && xs::device_slot_hits(73) == 1);
    EXPECT(mock->args[12][0] == 0 && mock->args[12][1] == D3D12_COMMAND_LIST_TYPE_DIRECT);
    EXPECT(mock->args[12][2] == word(real_allocator) && mock->args[12][3] == 0);
    // The PC list is created as a member with the proxy's own IID copy: the base GraphicsCommandList for the console
    // IID (measured: the PC SDK does not declare the console one, so asking for it returned E_NOINTERFACE and the
    // title asserted on a null list), or the title's PC IID unchanged. The pointer the real slot 12 receives is
    // therefore never the title's; the test compares the value it asked for.
    EXPECT(IsEqualIID(mock->list_iid, list_iid));

    // Release the list before the refusal calls below: the COM convention (and the handler) nulls the caller's
    // output on failure, so an invalid call would clear this pointer.
    EXPECT(release(list) == 0);

    // Missing description, allocator or output: E_INVALIDARG/E_POINTER before the real device is called.
    EXPECT(hr_of(raw(device, 73, 0, word(allocator), 0, word(&list_iid), word(&list))) == E_INVALIDARG);
    EXPECT(hr_of(raw(device, 73, word(&desc), 0, 0, word(&list_iid), word(&list))) == E_INVALIDARG);
    EXPECT(hr_of(raw(device, 73, word(&desc), word(allocator), 0, word(&list_iid), 0)) == E_POINTER);

    EXPECT(xs::device_slot_overridden(48) && xs::device_slot_overridden(73) && xs::device_slot_overridden(78) &&
           xs::device_slot_overridden(79) && xs::device_slot_overridden(80));

    EXPECT(release(allocator) == 0 && real_allocator->refs == 0);
    EXPECT(release(queue) == 0);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: device slots 48/73/78/79/80 route to a queue, a list and frame-event tokens without reaching "
                "the mock's own PC slots\n", g_label);
    return 0;
#else
    std::printf("%s: ID3D12Device11 is not in this SDK; Xbox slot 80's beyond-table case is not built\n", g_label);
    return 0;
#endif
}

// ---- 7c. Console command-signature descriptions at device slot 41 (docs/command_signature.md) ----

int check_console_command_signature() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 11, true);
    EXPECT(device);
    xs::reset_device_proxy_stats();
    EXPECT(xs::device_slot_overridden(41));
    const IID signature_iid = __uuidof(ID3D12CommandSignature);

    // The first two measured descriptions are PC descriptions (DRAW / stride 16 and DRAW_INDEXED / stride 20): the host
    // receives the title's own pointer, so a description with no console-only type is not copied or touched.
    {
        const struct {
            D3D12_INDIRECT_ARGUMENT_TYPE type;
            UINT stride;
        } shapes[] = {{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW, sizeof(D3D12_DRAW_ARGUMENTS)},
                      {D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED, sizeof(D3D12_DRAW_INDEXED_ARGUMENTS)}};
        for (unsigned i = 0; i < std::size(shapes); ++i) {
            D3D12_INDIRECT_ARGUMENT_DESC arg{};
            arg.Type = shapes[i].type;
            D3D12_COMMAND_SIGNATURE_DESC desc{};
            desc.ByteStride = shapes[i].stride;
            desc.NumArgumentDescs = 1;
            desc.pArgumentDescs = &arg;
            void* signature = nullptr;
            EXPECT(hr_of(raw(device, 41, word(&desc), 0, word(&signature_iid), word(&signature))) == S_OK);
            EXPECT(signature && mock->calls[41] == i + 1);
            EXPECT(mock->signature_desc_pointer == &desc);
            EXPECT(mock->signature_arg_count == 1 && mock->signature_args[0].Type == shapes[i].type);
            EXPECT(xs::device_slot_hits(41) == i + 1);
            EXPECT(release(signature) == 0);
        }
    }

    // The third measured description carries console argument type 105 with a 12-byte stride. The host is handed the
    // proxy's copy with DISPATCH in that argument, the stride and NodeMask unchanged and no root signature, and the
    // title's own description is not modified.
    {
        D3D12_INDIRECT_ARGUMENT_DESC arg{};
        arg.Type = static_cast<D3D12_INDIRECT_ARGUMENT_TYPE>(105);
        D3D12_COMMAND_SIGNATURE_DESC desc{};
        desc.ByteStride = 12;
        desc.NumArgumentDescs = 1;
        desc.pArgumentDescs = &arg;
        void* signature = nullptr;
        EXPECT(hr_of(raw(device, 41, word(&desc), 0, word(&signature_iid), word(&signature))) == S_OK);
        EXPECT(signature && mock->calls[41] == 3);
        EXPECT(mock->signature_desc_pointer != &desc);
        EXPECT(mock->signature_desc.ByteStride == 12 && mock->signature_desc.NumArgumentDescs == 1 &&
               mock->signature_desc.NodeMask == 0);
        EXPECT(mock->signature_arg_count == 1 &&
               mock->signature_args[0].Type == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH);
        EXPECT(static_cast<std::uint32_t>(arg.Type) == 105);
        EXPECT(mock->signature_root == 0);
        EXPECT(release(signature) == 0);
    }

    // A console argument type the measurement does not name (106) is not translated: the host receives the title's own
    // description and refuses it with E_INVALIDARG - the shim cannot turn an unmeasured type into a success.
    {
        D3D12_INDIRECT_ARGUMENT_DESC arg{};
        arg.Type = static_cast<D3D12_INDIRECT_ARGUMENT_TYPE>(106);
        D3D12_COMMAND_SIGNATURE_DESC desc{};
        desc.ByteStride = 12;
        desc.NumArgumentDescs = 1;
        desc.pArgumentDescs = &arg;
        void* signature = reinterpret_cast<void*>(0x1);
        EXPECT(hr_of(raw(device, 41, word(&desc), 0, word(&signature_iid), word(&signature))) == E_INVALIDARG);
        EXPECT(!signature && mock->signature_desc_pointer == &desc);
    }

    // The fifth measured description is DISPATCH_MESH with a 12-byte stride. The layer carries no mesh command
    // signature, so the proxy answers with a named dispatch-shaped stand-in (the host sees DISPATCH) and records the
    // object it handed out.
    void* standin = nullptr;
    {
        D3D12_INDIRECT_ARGUMENT_DESC arg{};
        arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
        D3D12_COMMAND_SIGNATURE_DESC desc{};
        desc.ByteStride = 12;
        desc.NumArgumentDescs = 1;
        desc.pArgumentDescs = &arg;
        EXPECT(hr_of(raw(device, 41, word(&desc), 0, word(&signature_iid), word(&standin))) == S_OK);
        EXPECT(standin != nullptr && xs::command_signature_standin(standin));
        EXPECT(mock->signature_args[0].Type == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH);
    }

    // ExecuteIndirect (console list slot 61) that names the stand-in is refused by name and not forwarded to the host
    // list's ExecuteIndirect (PC slot 59); any other signature is forwarded.
    {
        const IID allocator_iid = __uuidof(ID3D12CommandAllocator);
        void* allocator = nullptr;
        EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&allocator))) == S_OK);
        struct XboxListDesc {
            std::uint32_t type;
            std::uint32_t node_mask;
            std::uint64_t reserved[2];
        } list_desc{};
        list_desc.type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        const IID list_iid = __uuidof(ID3D12GraphicsCommandList);
        void* list = nullptr;
        EXPECT(hr_of(raw(device, 73, word(&list_desc), word(allocator), 0, word(&list_iid), word(&list))) == S_OK);
        Mock* real_list = reinterpret_cast<Mock*>(xs::command_list_proxy_real_list(list));
        EXPECT(real_list && real_list->kind == Kind::command_list);
        raw(list, 61, word(standin), 1, 0, 0, 0, 0);
        EXPECT(xs::list_slot_hits(61) == 1 && real_list->calls[59] == 0);
        D3D12_INDIRECT_ARGUMENT_DESC draw_arg{};
        draw_arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
        D3D12_COMMAND_SIGNATURE_DESC draw_desc{};
        draw_desc.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
        draw_desc.NumArgumentDescs = 1;
        draw_desc.pArgumentDescs = &draw_arg;
        void* signature = nullptr;
        EXPECT(hr_of(raw(device, 41, word(&draw_desc), 0, word(&signature_iid), word(&signature))) == S_OK);
        EXPECT(!xs::command_signature_standin(signature));
        raw(list, 61, word(signature), 1, 0, 0, 0, 0);
        EXPECT(xs::list_slot_hits(61) == 2 && real_list->calls[59] == 1);
        EXPECT(release(signature) == 0);
        EXPECT(release(list) == 0 && release(allocator) == 0);
    }

    EXPECT(release(standin) == 0);
    EXPECT(xs::device_slot_hits(41) == 6);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: console slot 41 forwards the measured DRAW/DRAW_INDEXED descriptions, creates console argument "
                "type 105 as DISPATCH, forwards an unmeasured type to the host's refusal, and refuses ExecuteIndirect "
                "with the DISPATCH_MESH stand-in by name\n", g_label);
    return 0;
}

// ---- 7b. The measured console slot 74 and the slot-25 depth-stencil + unordered-access stand-in ----

int check_slot74_and_slot25_standin() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 11, true);
    EXPECT(device);
    xs::reset_device_proxy_stats();

    // Slot 74: the title calls console slot 74 with three words (pointer, 0x185c0, pointer) at game+0x1e6c2c1 and
    // asserts at game+0x1e6c2c5 when the answer is negative. The PC method at this vtable index is
    // ID3D12Device9::ShaderCacheControl with another shape, so the proxy answers S_OK and never reaches it.
    EXPECT(hr_of(raw(device, 74, 0x1000, 0x185c0, 0x2000)) == S_OK);
    EXPECT(mock->calls[74] == 0 && xs::device_slot_hits(74) == 1);
    EXPECT(xs::device_slot_overridden(74));

    // Slot 25: a depth-stencil texture that also allows unordered access (the measured pair). The PC runtime refuses
    // the pair with SizeInBytes = UINT64_MAX; the console allows it, so the stand-in re-queries the same description
    // with ALLOW_UNORDERED_ACCESS cleared and returns that size. Format 19 is the title's measured depth-family
    // description.
    D3D12_RESOURCE_DESC depth_uav{};
    depth_uav.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depth_uav.Width = 128;
    depth_uav.Height = 128;
    depth_uav.DepthOrArraySize = 1;
    depth_uav.MipLevels = 1;
    depth_uav.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    depth_uav.SampleDesc.Count = 1;
    depth_uav.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depth_uav.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_RESOURCE_ALLOCATION_INFO info{};
    info.SizeInBytes = 0xA5A5A5A5u;
    EXPECT(raw(device, 25, word(&info), 0, 1, word(&depth_uav)) == word(&info));
    EXPECT(info.SizeInBytes != UINT64_MAX && info.Alignment == D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
    EXPECT(mock->allocation_queries == 2);  // asked with the pair (refused), then without the flag (sized)
    EXPECT(static_cast<std::uint32_t>(mock->allocation_flags) ==
           static_cast<std::uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL));
    EXPECT(xs::device_slot_hits(25) == 1);

    // A description the retry still refuses (the mock refuses a zero width) keeps the host's invalid allocation: the
    // stand-in cannot hide a genuine refusal.
    D3D12_RESOURCE_DESC still_refused = depth_uav;
    still_refused.Width = 0;
    info.SizeInBytes = 0;
    EXPECT(raw(device, 25, word(&info), 0, 1, word(&still_refused)) == word(&info));
    EXPECT(info.SizeInBytes == UINT64_MAX);
    EXPECT(mock->allocation_queries == 2);  // the retry did not re-size a description the host already refused

    // A plain colour texture is answered by the first query: the stand-in costs nothing on a description the host sizes.
    D3D12_RESOURCE_DESC colour = depth_uav;
    colour.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    colour.Flags = D3D12_RESOURCE_FLAG_NONE;
    const unsigned queries_before = mock->allocation_queries;
    info.SizeInBytes = 0;
    EXPECT(raw(device, 25, word(&info), 0, 1, word(&colour)) == word(&info));
    EXPECT(info.SizeInBytes == 0x10000 && mock->allocation_queries == queries_before + 1);

    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: console slot 74 answers the measured three-word call S_OK, and slot 25 sizes a depth-stencil "
                "texture that also allows unordered access by re-querying without the flag\n",
                g_label);
    return 0;
}


int run_env_slots(bool valid) {
    const char* frame_interval = valid ? "96" : "2";
    const char* schedule_event = valid ? "97" : "2";
    const char* wait_event = valid ? "98" : "2";
    const char* queue_x = valid ? "99" : "2";
    const char* list_x = valid ? "100" : "2";
    EXPECT(SetEnvironmentVariableA("PALMETTO_XS_FRAME_INTERVAL_SLOT", frame_interval) != FALSE);
    EXPECT(SetEnvironmentVariableA("PALMETTO_XS_SCHEDULE_EVENT_SLOT", schedule_event) != FALSE);
    EXPECT(SetEnvironmentVariableA("PALMETTO_XS_WAIT_EVENT_SLOT", wait_event) != FALSE);
    EXPECT(SetEnvironmentVariableA("PALMETTO_XS_QUEUE_X_SLOT", queue_x) != FALSE);
    EXPECT(SetEnvironmentVariableA("PALMETTO_XS_LIST_X_SLOT", list_x) != FALSE);

    Mock* mock = nullptr;
    void* device = make_device(mock, 11, true);
    EXPECT(device);
    xs::reset_device_proxy_stats();
    std::uint64_t token = 0;
    if (valid) {
        // Every route moved: the new slot is handled, the default slot is forwarded to the mock.
        EXPECT(hr_of(raw(device, 96, 0, 16667, 1, 0)) == S_OK && mock->calls[96] == 0);
        EXPECT(hr_of(raw(device, 97, 0, 0, 0, 0)) == S_OK && mock->calls[97] == 0);
        EXPECT(hr_of(raw(device, 98, 0, 0, 0, 0, word(&token))) == S_OK && token && mock->calls[98] == 0);
        void* queue = nullptr;
        EXPECT(hr_of(raw(device, 99, word(&kQueueDesc), word(&kQueueIid), word(&queue))) == S_OK);
        EXPECT(xs::is_queue_proxy(queue) && mock->calls[99] == 0);
        const IID allocator_iid = __uuidof(ID3D12CommandAllocator);
        void* allocator = nullptr;
        EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&allocator))) == S_OK);
        struct {
            std::uint32_t type, node_mask;
        } desc{static_cast<std::uint32_t>(D3D12_COMMAND_LIST_TYPE_DIRECT), 0};
        const IID list_iid = __uuidof(ID3D12GraphicsCommandList);
        void* list = nullptr;
        EXPECT(hr_of(raw(device, 100, word(&desc), word(allocator), 0, word(&list_iid), word(&list))) == S_OK);
        EXPECT(hr_of(raw(device, 78, 0, 16667, 1, 0)) == S_OK && mock->calls[78] == 1);
        EXPECT(release(list) == 0);
        EXPECT(release(allocator) == 0);
        EXPECT(release(queue) == 0);
    } else {
        // Values outside 3..127 are ignored: the five defaults still answer, and a PC slot that is not a default is
        // forwarded unchanged.
        EXPECT(hr_of(raw(device, 78, 0, 16667, 1, 0)) == S_OK && mock->calls[78] == 0);
        EXPECT(hr_of(raw(device, 79, 0, 0, 0, 0)) == S_OK && mock->calls[79] == 0);
        EXPECT(hr_of(raw(device, 80, 0, 0, 0, 0, word(&token))) == S_OK && token && mock->calls[80] == 0);
        // 70 is measured as a console slot now: the title's thunk calls it with three arguments, so the proxy refuses
        // it (measured by console-measured arity) instead of forwarding it to the PC CreatePlacedResource1. The
        // forwarded-slot check therefore uses 77: inside the 80-slot PC vtable and not a slot the proxy overrides
        // (74 is now the measured console three-word slot).
        EXPECT(hr_of(raw(device, 70)) == E_NOTIMPL && mock->calls[70] == 0);
        EXPECT(hr_of(raw(device, 77)) == S_OK && mock->calls[77] == 1);  // not an Xbox slot: forwarded unchanged
    }
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: environment slot overrides %s\n", g_label,
                valid ? "move all five routes" : "outside 3..127 are ignored");
    return 0;
}

// ---- 9. The refused tail and the guard region ----

int check_tail(bool want_guarded) {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    void* queue = make_queue(device);
    void* fence = make_fence(device);
    EXPECT(queue && fence);
    EXPECT(xs::command_object_tables_guarded() == want_guarded);
    Mock* const queue_mock = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue));
    // Signal is console slot 16 and reaches PC Signal (real slot 14), with the real fence unwrapped.
    EXPECT(hr_of(raw(queue, 16, word(fence), 5)) == S_OK && queue_mock->calls[14] == 1 &&
           queue_mock->args[14][0] == word(xs::fence_proxy_real_fence(fence)) && queue_mock->args[14][1] == 5);
    // Slot 35 is no longer part of the refused tail: the title calls it at two measured sites as (this, arg, pair*),
    // and the proxy answers a clock calibration pair instead of E_NOTIMPL (check_queue_console_slots checks the pair
    // itself). Slot 32 is the console PresentX: it goes through the presentation's registration hook
    // (docs/present_wiring.md), and with nothing registered in this GPU-free binary the hook refuses with
    // HRESULT_FROM_WIN32(ERROR_INVALID_STATE) without touching the real queue. Slot 31 (CopyPageMappingsBatchX) is
    // answered through xmem's mapping hook when a build links xmem; here that hook is null, so it is refused like the
    // rest of the tail (check_queue_copy_page_mappings installs its own recorder and restores this answer). The rest of
    // the tail keeps the refused behaviour, and never reaches the real queue.
    for (unsigned slot = 21; slot < xs::queue_proxy_table_size(); ++slot) {  // 16..20 map to PC 14..18
        if (slot == 32 || slot == 35) continue;
        EXPECT(hr_of(raw(queue, slot)) == E_NOTIMPL && queue_mock->calls[slot] == 0);
    }
    EXPECT(hr_of(raw(queue, 32)) == HRESULT_FROM_WIN32(ERROR_INVALID_STATE) && queue_mock->calls[32] == 0);
    EXPECT(hr_of(raw(queue, 35)) == S_OK && queue_mock->calls[35] == 0);
    // Console fence slot 12 is answered (the completed-value cell, check_fence1); the refused tail starts at 13.
    for (unsigned slot = 13; slot < xs::fence_proxy_table_size(); ++slot) EXPECT(hr_of(raw(fence, slot)) == E_NOTIMPL);
    EXPECT(xs::queue_table_overruns() == 0 && xs::fence_table_overruns() == 0);
    EXPECT(release(queue) == 0 && release(fence) == 0 && release(device) == 0 && mock->refs == 0);
    return 0;
}

// ---- 10. Queue slot 18 (GetDesc) and slot 35 (the console calibration pair) ----

int check_queue_console_slots() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    void* queue = make_queue(device);
    EXPECT(xs::is_queue_proxy(queue) && xs::queue_proxy_slot_count(queue) == 19);
    Mock* const real_queue = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue));
    EXPECT(real_queue && real_queue->kind == Kind::queue);
    xs::reset_device_proxy_stats();

    // The console queue inserts two methods before Signal (docs/d3d12_xs_surface.md's PresentX slot-30/32 split), so
    // console slots 16..20 name PC slots 14..18. Measured on the title: Signal at console 16, GetTimestampFrequency at
    // console 18, GetDesc at console 20. Forwarded by number, console Signal reached PC GetTimestampFrequency, which
    // stored 1e9 through the fence it was given - that is the corruption this mapping exists to stop.
    EXPECT(hr_of(raw(queue, 16)) == S_OK && real_queue->calls[14] == 1);   // Signal -> PC Signal
    EXPECT(hr_of(raw(queue, 17)) == S_OK && real_queue->calls[15] == 1);   // Wait -> PC Wait
    EXPECT(hr_of(raw(queue, 18)) == S_OK && real_queue->calls[16] == 1);   // GetTimestampFrequency
    EXPECT(hr_of(raw(queue, 19)) == S_OK && real_queue->calls[17] == 1);   // GetClockCalibration
    // The two inserted methods are not identified, so they must refuse rather than land on PC Signal/Wait.
    const unsigned signal_before = real_queue->calls[14];
    EXPECT(hr_of(raw(queue, 14)) == E_NOTIMPL && real_queue->calls[14] == signal_before);
    EXPECT(hr_of(raw(queue, 15)) == E_NOTIMPL && real_queue->calls[15] == 1);

    // Slot 20, GetDesc (PC slot 18). Measured: the title calls it as (queue in RCX, buffer in RDX), while our real queue
    // is a C++
    // object whose GetDesc returns D3D12_COMMAND_QUEUE_DESC by value, so MSVC's method takes the hidden return buffer
    // in RCX and the object in RDX. Forwarded through the generic path the descriptor landed in the front of the real
    // queue object and the title's buffer kept its old contents (measured: the title read 1 << 32 from it and faulted
    // on 0x100000000), so the proxy must swap the two.
    alignas(16) std::uint8_t buffer[sizeof(D3D12_COMMAND_QUEUE_DESC)];
    std::memset(buffer, 0xcd, sizeof(buffer));  // a stale buffer, not a descriptor
    std::uint8_t front_before[16], front_after[16];
    std::memcpy(front_before, real_queue, sizeof(front_before));
    real_queue->get_desc_hidden = real_queue->get_desc_self = 0;
    const unsigned desc_calls_before = real_queue->calls[18];
    const unsigned desc_hits_before = xs::queue_slot_hits(20);
    EXPECT(hr_of(raw(queue, 20, word(buffer))) == S_OK);
    std::memcpy(front_after, real_queue, sizeof(front_after));
    EXPECT(xs::queue_slot_hits(20) == desc_hits_before + 1 && real_queue->calls[18] == desc_calls_before + 1);
    // (a) the hidden return pointer is the buffer the title passed in RDX...
    EXPECT(real_queue->get_desc_hidden == word(buffer));
    // (b) ...and the object pointer is the mock real queue, not the title's buffer.
    EXPECT(real_queue->get_desc_self == word(real_queue));
    // (c) the 16-byte descriptor lands in the title's buffer.
    EXPECT(std::memcmp(buffer, &kMockQueueDesc, sizeof(kMockQueueDesc)) == 0);
    // (d) the real queue object's own first 16 bytes are untouched (the old order wrote the descriptor there).
    EXPECT(std::memcmp(front_before, front_after, sizeof(front_before)) == 0);

    // Slot 35 is the console's submit (docs/render_loop.md): the title calls it as (count, list array, frame token,
    // reserved) and there is no ExecuteCommandLists call in the whole boot. The proxy forwards count and the unwrapped
    // array to PC ExecuteCommandLists (real slot 10). The old "clock calibration pair" answer wrote two QPC words into
    // that array, i.e. into the title's list pointers.
    std::uint64_t lists[2] = {0, 0};
    const unsigned execute_before = real_queue->calls[10];
    EXPECT(hr_of(raw(queue, 35, 2, word(lists), 0x1122334455667788)) == S_OK);
    EXPECT(real_queue->calls[10] == execute_before + 1);
    // The array is copied with its entries unwrapped, so the forwarded pointer is the proxy's copy, not the caller's
    // array; what matters is the count and that a non-null array reached ExecuteCommandLists.
    EXPECT(real_queue->args[10][0] == 2 && real_queue->args[10][1] != 0);
    EXPECT(lists[0] == 0 && lists[1] == 0);  // no write into the caller's array
    // A null array with count 0 is still a submit call, not a write.
    EXPECT(hr_of(raw(queue, 35, 0, 0)) == S_OK);
    EXPECT(real_queue->calls[10] == execute_before + 2);
    EXPECT(xs::queue_slot_hits(35) == 2 && real_queue->calls[35] == 0);  // answered by the proxy, never forwarded

    EXPECT(release(queue) == 0 && real_queue->refs == 0);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: the console queue's two-method shift maps Signal/Wait/GetTimestampFrequency/GetClockCalibration/"
                "GetDesc from slots 16..20 to PC 14..18, the inserted methods refuse, slot 20 converts GetDesc's console "
                "struct-return order (the title's buffer gets the descriptor, the real queue is the object, its front "
                "words untouched) and slot 35 answers a calibration "
                "pair when the out pointer is writable and S_OK without writing otherwise\n", g_label);
    return 0;
}

// ---- 11. Queue slot 31: CopyPageMappingsBatchX ----
//
// Our own static measurement (docs/cpu_view_mapping.md): game+0xf2cc7f calls console queue slot 31 with (batch count,
// batch array, pool base VA, pool page count, flags = 0) and does not test the answer; a batch is 24 bytes
// {destination, range count, ranges} and a range 12 bytes {kind, start, count} (rt::xmem::PageBatch/PageRange). The page
// mapping itself needs xmem, which this GPU-free binary does not link, so the proxy reaches it through
// rt::xmem::copy_page_mappings_fn; these checks pin the console argument decode and the refusal bookkeeping with a
// recorder in its place (xmem_test covers the host mapping that recorder stands for).

struct RemapCall {
    std::uint64_t destination = 0;
    std::uint64_t source = 0;
    std::uint64_t pages = 0;
    bool repeat_first = false;
};
constexpr unsigned kMaxRemaps = 16;
RemapCall g_remaps[kMaxRemaps];
unsigned g_remap_calls = 0;
bool g_remap_result = true;

bool record_remap(void* destination, const void* source, std::uint64_t pages, bool repeat_first) {
    if (g_remap_calls < kMaxRemaps)
        g_remaps[g_remap_calls] = RemapCall{word(destination), word(source), pages, repeat_first};
    ++g_remap_calls;
    return g_remap_result;
}

void clear_remaps() {
    g_remap_calls = 0;
    g_remap_result = true;
}

int check_queue_copy_page_mappings() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    void* queue = make_queue(device);
    EXPECT(queue);
    Mock* const real_queue = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue));
    EXPECT(real_queue && real_queue->kind == Kind::queue);
    constexpr std::uint64_t kPage = 0x10000;
    const std::uint64_t pool = 0x7f0000000000ull;
    const std::uint64_t destination = 0x7f1000000000ull;
    rt::xmem::PageRange ranges[5] = {{0, 1, 2}, {1, 3, 2}, {2, 0, 1}, {3, 0, 5}, {4, 0, 1}};
    rt::xmem::PageBatch batches[2] = {{destination, 4, ranges}, {destination + 0x300000, 2, &ranges[2]}};
    const unsigned hits_before = xs::queue_slot_hits(31);

    // No mapper in this binary: the call keeps the answer every unmodelled queue slot past the PC chain gets, is
    // counted and traced, and never reaches the real queue (check_tail walks every refused tail slot the same way).
    EXPECT(!rt::xmem::copy_page_mappings_fn);
    EXPECT(hr_of(raw(queue, 31, 1, word(&batches[0]), pool, 4)) == E_NOTIMPL);
    EXPECT(real_queue->calls[31] == 0 && xs::queue_slot_hits(31) == hits_before + 1);

    rt::xmem::copy_page_mappings_fn = &record_remap;
    clear_remaps();
    // A two-batch call: kind 0 maps pool pages start, start + 1, ... at the destination; kind 1 the one pool page
    // `start` at every destination page; kind 2 unmaps; kind 3 changes nothing and calls nothing. Each range moves the
    // batch cursor on by its page count, and each batch starts at its own destination.
    EXPECT(hr_of(raw(queue, 31, 2, word(batches), pool, 4)) == S_OK);
    EXPECT(real_queue->calls[31] == 0);
    EXPECT(g_remap_calls == 4);
    EXPECT(g_remaps[0].destination == destination && g_remaps[0].source == pool + 1 * kPage &&
           g_remaps[0].pages == 2 && !g_remaps[0].repeat_first);
    EXPECT(g_remaps[1].destination == destination + 2 * kPage && g_remaps[1].source == pool + 3 * kPage &&
           g_remaps[1].pages == 2 && g_remaps[1].repeat_first);
    EXPECT(g_remaps[2].destination == destination + 4 * kPage && g_remaps[2].source == 0 && g_remaps[2].pages == 1 &&
           !g_remaps[2].repeat_first);
    EXPECT(g_remaps[3].destination == destination + 0x300000 && g_remaps[3].source == 0 && g_remaps[3].pages == 1 &&
           !g_remaps[3].repeat_first);
    EXPECT(batches[0].destination == destination && ranges[0].kind == 0 && ranges[1].start == 3);

    // A kind 3-only call is S_OK and maps nothing.
    clear_remaps();
    rt::xmem::PageBatch unchanged{destination, 1, &ranges[3]};
    EXPECT(hr_of(raw(queue, 31, 1, word(&unchanged), pool, 4)) == S_OK && g_remap_calls == 0);

    // Refusals: a pooled run past the pool, an unknown kind, a null pool, a hook that refuses, a null batch array and a
    // batch whose range count has a null array. Each returns E_INVALIDARG (the title reads the destinations anyway, so
    // the ranges after a refused one in the same call are still applied) and the answered call never reaches the queue.
    clear_remaps();
    rt::xmem::PageRange past_pool[2] = {{0, 3, 2}, {2, 0, 1}};
    rt::xmem::PageBatch refused{destination, 2, past_pool};
    EXPECT(hr_of(raw(queue, 31, 1, word(&refused), pool, 4)) == E_INVALIDARG && g_remap_calls == 1);
    clear_remaps();
    rt::xmem::PageBatch unknown{destination, 1, &ranges[4]};
    EXPECT(hr_of(raw(queue, 31, 1, word(&unknown), pool, 4)) == E_INVALIDARG && g_remap_calls == 0);
    clear_remaps();
    EXPECT(hr_of(raw(queue, 31, 1, word(&batches[0]), 0 /* no pool */, 4)) == E_INVALIDARG && g_remap_calls == 1);
    clear_remaps();
    g_remap_result = false;
    rt::xmem::PageBatch refused_by_hook{destination, 1, &ranges[2]};
    EXPECT(hr_of(raw(queue, 31, 1, word(&refused_by_hook), pool, 4)) == E_INVALIDARG && g_remap_calls == 1);
    clear_remaps();
    EXPECT(hr_of(raw(queue, 31, 1, 0 /* null batches with a count */, pool, 4)) == E_INVALIDARG);
    rt::xmem::PageBatch null_ranges{destination, 1, nullptr};
    EXPECT(hr_of(raw(queue, 31, 1, word(&null_ranges), pool, 4)) == E_INVALIDARG && g_remap_calls == 0);
    EXPECT(hr_of(raw(queue, 31, 0, 0 /* count 0: nothing to map */, pool, 0)) == S_OK && g_remap_calls == 0);

    // Clearing the hook restores the unmodelled answer.
    rt::xmem::copy_page_mappings_fn = nullptr;
    EXPECT(hr_of(raw(queue, 31, 1, word(&batches[0]), pool, 4)) == E_NOTIMPL && real_queue->calls[31] == 0);

    EXPECT(release(queue) == 0 && real_queue->refs == 0);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: console queue slot 31 decodes the measured (count, batches, pool, pool pages, flags) call into "
                "xmem's page-mapping hook (kinds 0/1/2/3, batch cursor and destinations), refuses a run past the pool, "
                "an unknown kind, a null pool/array and a hook that refuses with E_INVALIDARG, and keeps the "
                "unmodelled E_NOTIMPL without a mapper\n", g_label);
    return 0;
}

// ---- Guard-region setup failures: this executable's import table, patched before the first proxy table ----

std::atomic<int> g_injected{0};
bool g_armed = false;
void* g_real = nullptr;

LPVOID WINAPI fail_virtual_alloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect) {
    if (g_armed && (type & MEM_RESERVE) && size == 64 * 1024) {
        ++g_injected;
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    return reinterpret_cast<decltype(&VirtualAlloc)>(g_real)(address, size, type, protect);
}

BOOL WINAPI fail_virtual_protect(LPVOID address, SIZE_T size, DWORD protect, PDWORD old) {
    if (g_armed && protect == PAGE_READONLY) {
        ++g_injected;
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return reinterpret_cast<decltype(&VirtualProtect)>(g_real)(address, size, protect, old);
}

PVOID WINAPI fail_add_veh(ULONG, PVECTORED_EXCEPTION_HANDLER) {
    ++g_injected;
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return nullptr;
}

// Points this executable's import of `function` (by name) at `replacement`; the proxy code is linked into it.
bool patch_import(const char* function, void* replacement) {
    auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress) return false;
    for (auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress); desc->Name; ++desc) {
        const DWORD lookup = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + lookup);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), function) != 0) continue;
            DWORD old = 0;
            if (!VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return false;
            g_real = reinterpret_cast<void*>(slots->u1.Function);
            slots->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
            VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);  // through the replacement, not armed yet
            return true;
        }
    }
    return false;
}

int run_fault_mode(const char* mode) {
    struct Mode { const char* flag; const char* function; void* replacement; bool guarded; } const modes[] = {
        {"--fail-virtualalloc", "VirtualAlloc", reinterpret_cast<void*>(&fail_virtual_alloc), false},
        {"--fail-virtualprotect", "VirtualProtect", reinterpret_cast<void*>(&fail_virtual_protect), true},
        {"--fail-veh", "AddVectoredExceptionHandler", reinterpret_cast<void*>(&fail_add_veh), false},
    };
    for (const Mode& m : modes) {
        if (std::strcmp(mode, m.flag) != 0) continue;
        g_label = m.flag;
        EXPECT(patch_import(m.function, m.replacement));
        g_armed = true;
        const int failed = check_tail(m.guarded);
        // Disarmed once the tables exist: with a static CRT the CRT's own calls go through this import table too
        // (its exit path asks VirtualProtect for read-only pages), and only the proxy's calls are meant here.
        g_armed = false;
        if (failed) return 1;
        EXPECT(g_injected.load() > 0);  // the proxy's own call was the one refused
        std::printf("%s: %s refused %d time(s): the tables %s guarded, the proxies forward and refuse their tail as "
                    "before\n", m.flag, m.function, g_injected.load(), m.guarded ? "stay" : "are not");
        return 0;
    }
    return 2;
}

}  // namespace

// A failure ends the process with a message on stderr, never in a dialog a test run would wait on: abort() (an
// exception that escapes, for one), the debug CRT's reports, and Windows' fault and critical-error boxes.
static void no_dialogs() {
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#ifdef _DEBUG  // the report functions exist only in the debug CRT (empty macros otherwise)
    for (const int type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
    }
#endif
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
}

int check_console_command_list() {
    // 1. Slot mapping checks
    EXPECT(xs::console_to_pc_list_slot(14) == 14);   // Dispatch
    EXPECT(xs::console_to_pc_list_slot(26) == 26);   // ResourceBarrier
    EXPECT(xs::console_to_pc_list_slot(31) == 27);   // ExecuteBundle
    EXPECT(xs::console_to_pc_list_slot(32) == 28);   // SetDescriptorHeaps
    EXPECT(xs::console_to_pc_list_slot(33) == 29);   // SetComputeRootSignature
    EXPECT(xs::console_to_pc_list_slot(35) == 31);   // SetComputeRootDescriptorTable
    EXPECT(xs::console_to_pc_list_slot(60) == 58);   // EndEvent
    EXPECT(xs::console_to_pc_list_slot(61) == 59);   // ExecuteIndirect
    EXPECT(xs::console_to_pc_list_slot(85) == 16);   // CopyTextureRegion
    EXPECT(xs::console_to_pc_list_slot(146) == 80);  // GCL7 Barrier
    EXPECT(xs::console_to_pc_list_slot(148) == xs::kListNotForwarded); // AllocateUploadMemory

    Mock* mock_device = nullptr;
    void* device = make_device(mock_device, 11, true);
    EXPECT(device != nullptr);

    // 2. PC Slot 12 vs Xbox Slot 73
    // PC Slot 12 returns unwrapped list directly, so PC tests like tiny_frame can call slot 48 (ClearRenderTargetView)
    void* pc_list = nullptr;
    const IID list_iid = __uuidof(ID3D12GraphicsCommandList);
    EXPECT(hr_of(raw(device, 12, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, word(&list_iid), word(&pc_list))) == S_OK);
    EXPECT(pc_list != nullptr);
    EXPECT(!xs::is_command_list_proxy(pc_list));
    Mock* real_pc_list = reinterpret_cast<Mock*>(pc_list);
    EXPECT(real_pc_list->kind == Kind::command_list);
    // In PC ABI, slot 48 is ClearRenderTargetView
    raw(pc_list, 48, 0x1111, 0x2222, 0, 0);
    EXPECT(real_pc_list->calls[48] == 1);
    EXPECT(release(pc_list) == 0);

    // Xbox Slot 73 returns wrapped console command list proxy
    void* allocator = nullptr;
    const IID alloc_iid = __uuidof(ID3D12CommandAllocator);
    EXPECT(hr_of(raw(device, 9, 0, word(&alloc_iid), word(&allocator))) == S_OK);
    EXPECT(xs::is_allocator_proxy(allocator));
    Mock* real_allocator = reinterpret_cast<Mock*>(xs::allocator_proxy_real_allocator(allocator));

    struct XboxCommandListDesc {
        std::uint32_t type;
        std::uint32_t node_mask;
        std::uint64_t reserved[2];
    } desc{D3D12_COMMAND_LIST_TYPE_DIRECT, 0, {0, 0}};

    void* list_proxy = nullptr;
    EXPECT(hr_of(raw(device, 73, word(&desc), word(allocator), 0, word(&list_iid), word(&list_proxy))) == S_OK);
    EXPECT(xs::is_command_list_proxy(list_proxy));
    EXPECT(xs::command_list_proxy_table_size() == 160);

    // 3. Inline cursor pinning: +0x18, +0x28, +0x30
    const auto base = reinterpret_cast<std::uintptr_t>(list_proxy);
    std::uint64_t c18 = 0, c28 = 0, c30 = 0;
    std::memcpy(&c18, reinterpret_cast<const void*>(base + 0x18), sizeof(c18));
    std::memcpy(&c28, reinterpret_cast<const void*>(base + 0x28), sizeof(c28));
    std::memcpy(&c30, reinterpret_cast<const void*>(base + 0x30), sizeof(c30));
    const std::uint64_t expected_limit = static_cast<std::uint64_t>(base + 0x18 + 0x500);
    EXPECT(c18 == expected_limit);
    EXPECT(c28 == expected_limit);
    EXPECT(c30 == expected_limit);

    // 4. Console slot routing on ListProxy
    Mock* real_list = reinterpret_cast<Mock*>(xs::command_list_proxy_real_list(list_proxy));
    EXPECT(real_list != nullptr && real_list->kind == Kind::command_list);

    // 3b. The console command-list lifecycle (docs/list_lifecycle.md): the title creates and closes its lists during
    // setup and then records into them again every frame. On the console that is legal (the allocator owns the
    // recording memory); the PC runtime would drop every recording call into a closed list. The proxy therefore
    // resets the real list against the allocator it was created with before forwarding a recording call that reaches
    // it closed. Console slot 85 (CopyTextureRegionX -> PC slot 16) is the recording call used here; it is a slot the
    // rest of this check does not otherwise touch.
    EXPECT(!real_list->list_closed);  // CreateCommandList leaves the real PC list recording
    EXPECT(real_list->reset_result == S_OK);
    EXPECT(real_list->calls[10] == 0 && real_list->calls[16] == 0);

    // One Close leaves the real list closed; the next recording call gets exactly one Reset, against the real
    // allocator (not the allocator proxy the title holds), and then the call itself.
    EXPECT(hr_of(raw(list_proxy, 9, 0, 0, 0, 0)) == S_OK);
    EXPECT(real_list->list_closed);
    unsigned lifecycle_events = real_list->list_event_count;
    raw(list_proxy, 85, 0x51, 0x52, 0x53, 0x54);
    EXPECT(real_list->calls[10] == 1);
    EXPECT(real_list->calls[16] == 1);
    EXPECT(real_list->args[10][0] == word(real_allocator));
    EXPECT(real_list->list_event_count == lifecycle_events + 2);
    EXPECT(real_list->list_events[lifecycle_events] == 10);
    EXPECT(real_list->list_events[lifecycle_events + 1] == 16);
    EXPECT(!real_list->list_closed);

    // A second Close and a second recording call trigger a second Reset (the per-frame case), and the list is open
    // again afterwards.
    EXPECT(hr_of(raw(list_proxy, 9, 0, 0, 0, 0)) == S_OK);
    raw(list_proxy, 85, 0x61, 0, 0, 0);
    EXPECT(real_list->calls[10] == 2);
    EXPECT(real_list->calls[16] == 2);
    EXPECT(!real_list->list_closed);

    // A forwarded call before PC recording starts (GetType, PC slot 8) is not a recording call: it never reopens a
    // closed list and no Reset happens.
    EXPECT(hr_of(raw(list_proxy, 9, 0, 0, 0, 0)) == S_OK);
    const unsigned resets_after_close = real_list->calls[10];
    raw(list_proxy, 8, 0, 0, 0, 0);
    EXPECT(real_list->calls[8] == 1);
    EXPECT(real_list->calls[10] == resets_after_close);
    EXPECT(real_list->list_closed);

    // A failed Reset is reported (E_FAIL with the call that needed it) and the recording call is refused, not
    // forwarded: the real list stays closed and untouched and the caller gets the Reset's HRESULT.
    real_list->reset_result = E_FAIL;
    const unsigned events_before_failure = real_list->list_event_count;
    EXPECT(hr_of(raw(list_proxy, 85, 0x71, 0, 0, 0)) == E_FAIL);
    EXPECT(real_list->calls[10] == resets_after_close + 1);
    EXPECT(real_list->calls[16] == 2);
    EXPECT(real_list->list_closed);
    EXPECT(real_list->list_event_count == events_before_failure + 1);
    EXPECT(real_list->list_events[events_before_failure] == 10);

    // Once Reset reports success again, a later recording call reopens normally and is forwarded.
    real_list->reset_result = S_OK;
    raw(list_proxy, 85, 0x81, 0, 0, 0);
    EXPECT(real_list->calls[10] == resets_after_close + 2);
    EXPECT(real_list->calls[16] == 3);
    EXPECT(!real_list->list_closed);

    // Console slot 32 (SetDescriptorHeaps) -> PC slot 28
    // Pass descriptor heap proxy to ensure UnwrappedArray unwrap_object works
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    void* heap_proxy = nullptr;
    const IID heap_iid = __uuidof(ID3D12DescriptorHeap);
    EXPECT(hr_of(raw(device, 14, word(&heap_desc), word(&heap_iid), word(&heap_proxy))) == S_OK);
    EXPECT(heap_proxy != nullptr && xs::is_heap_proxy(heap_proxy));
    ID3D12DescriptorHeap* real_heap = xs::heap_proxy_real_heap(heap_proxy);
    void* heap_ptrs[2] = {heap_proxy, real_heap};
    raw(list_proxy, 32, 2, word(heap_ptrs), 0, 0);
    EXPECT(real_list->calls[28] == 1);
    EXPECT(real_list->args[28][0] == 2);
    // The heap proxy should have been unwrapped in the array passed to PC slot 28:
    EXPECT(real_list->array[28][0] == word(real_heap));
    EXPECT(real_list->array[28][1] == word(real_heap));
    EXPECT(xs::list_slot_hits(32) == 1);
    EXPECT(xs::list_slot_first_call_seen(32));

    // Console slot 33 (SetComputeRootSignature) -> PC slot 29
    raw(list_proxy, 33, 0x5555, 0, 0, 0);
    EXPECT(real_list->calls[29] == 1);
    EXPECT(real_list->args[29][0] == 0x5555);
    EXPECT(xs::list_slot_hits(33) == 1);
    EXPECT(xs::list_slot_first_call_seen(33));

    // Console slot 60 (EndEvent) -> PC slot 58
    raw(list_proxy, 60, 0, 0, 0, 0);
    EXPECT(real_list->calls[58] == 1);

    // Console slot 61 (ExecuteIndirect) -> PC slot 59
    raw(list_proxy, 61, 0x10, 0x20, 0x30, 0x40);
    EXPECT(real_list->calls[59] == 1);
    EXPECT(real_list->args[59][0] == 0x10);

    // Console slot 148 (AllocateUploadMemory stub) -> returns 0 without faulting
    EXPECT(raw(list_proxy, 148, 2048, 512, 0, 0) == 0);
    EXPECT(xs::list_slot_hits(148) == 1);
    EXPECT(xs::list_slot_first_call_seen(148));

    // Console slot 146 (Barrier) on base 60-slot list returns E_NOTIMPL (PC slot 80 >= 60)
    EXPECT(hr_of(raw(list_proxy, 146, 0, 0, 0, 0)) == E_NOTIMPL);

    // Console slot 10 (Reset) unwraps allocator proxy and re-pins inline cursor
    std::uint64_t zero = 0;
    std::memcpy(reinterpret_cast<void*>(base + 0x18), &zero, sizeof(zero));
    const unsigned resets_before_explicit = real_list->calls[10];
    EXPECT(hr_of(raw(list_proxy, 10, word(allocator), 0, 0, 0)) == S_OK);
    EXPECT(real_list->calls[10] == resets_before_explicit + 1);
    EXPECT(real_list->args[10][0] == word(real_allocator));
    std::memcpy(&c18, reinterpret_cast<const void*>(base + 0x18), sizeof(c18));
    EXPECT(c18 == expected_limit);

    // Console slot 7 (GetDevice) returns device proxy
    void* list_dev = nullptr;
    const IID dev_iid = __uuidof(ID3D12Device);
    const auto dev_refs_before = xs::device_proxy_ref_count(device);
    EXPECT(hr_of(raw(list_proxy, 7, word(&dev_iid), word(&list_dev))) == S_OK);
    EXPECT(list_dev == device);
    EXPECT(xs::device_proxy_ref_count(device) == dev_refs_before + 1);
    EXPECT(release(list_dev) == dev_refs_before);

    // Console slot 0 (QueryInterface)
    void* list_qi = nullptr;
    EXPECT(hr_of(raw(list_proxy, 0, word(&list_iid), word(&list_qi))) == S_OK);
    EXPECT(list_qi == list_proxy);
    EXPECT(release(list_qi) == 1);

    // 5. Queue slot 10 (ExecuteCommandLists) unwraps ListProxy in array
    void* queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC qdesc{};
    const IID queue_iid = __uuidof(ID3D12CommandQueue);
    EXPECT(hr_of(raw(device, 8, word(&qdesc), word(&queue_iid), word(&queue))) == S_OK);
    EXPECT(xs::is_queue_proxy(queue));
    Mock* real_queue = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue));

    Mock* unwrapped_list = new_mock(Kind::command_list);
    void* lists_to_exec[2] = {unwrapped_list, list_proxy};
    raw(queue, 10, 2, word(lists_to_exec), 0, 0);
    EXPECT(real_queue->calls[10] == 1);
    EXPECT(real_queue->args[10][0] == 2);
    EXPECT(real_queue->array[10][0] == word(unwrapped_list));
    EXPECT(real_queue->array[10][1] == word(real_list));

    // 6. Test with list_level = 7 (supporting ID3D12GraphicsCommandList7, 81 slots)
    mock_device->list_level = 7;
    void* list7_proxy = nullptr;
    EXPECT(hr_of(raw(device, 73, word(&desc), word(allocator), 0, word(&list_iid), word(&list7_proxy))) == S_OK);
    EXPECT(xs::is_command_list_proxy(list7_proxy));
    Mock* real_list7 = reinterpret_cast<Mock*>(xs::command_list_proxy_real_list(list7_proxy));
    EXPECT(real_list7 != nullptr);

    // 6a. Console slot 146 -> PC slot 80: the console's enhanced-barrier call. Its measured groups are PC-shaped
    // D3D12_BARRIER_GROUPs whose texture entries may carry no resource (the console's "every texture"; PC D3D12 refuses
    // those and records the list invalid). The proxy rewrites such an entry as a global barrier with the same
    // sync/access words, passes a real-resource entry through as a texture barrier, and refuses what it cannot
    // translate without forwarding it (docs/handles_barriers.md).
    D3D12_TEXTURE_BARRIER console_textures[2]{};
    constexpr std::uint32_t kAllSubresources = 0xffffffffu;  // D3D12_BARRIER_SUBRESOURCE_RANGE: every subresource
    console_textures[0].SyncBefore = D3D12_BARRIER_SYNC_ALL_SHADING;
    console_textures[0].SyncAfter = D3D12_BARRIER_SYNC_RENDER_TARGET;
    console_textures[0].AccessBefore = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
    console_textures[0].AccessAfter = D3D12_BARRIER_ACCESS_RENDER_TARGET;
    console_textures[0].LayoutBefore = D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS;
    console_textures[0].LayoutAfter = D3D12_BARRIER_LAYOUT_RENDER_TARGET;
    console_textures[0].Subresources.IndexOrFirstMipLevel = kAllSubresources;
    console_textures[1] = console_textures[0];
    console_textures[1].pResource = reinterpret_cast<ID3D12Resource*>(static_cast<std::uintptr_t>(0x1dfa7088200ull));
    console_textures[1].AccessBefore = D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
    console_textures[1].LayoutBefore = D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
    D3D12_BARRIER_GROUP console_group{};
    console_group.Type = D3D12_BARRIER_TYPE_TEXTURE;
    console_group.NumBarriers = 2;
    console_group.pTextureBarriers = console_textures;
    raw(list7_proxy, 146, 1, word(&console_group), 0, 0);
    EXPECT(real_list7->calls[80] == 1);
    // The resource-less entry became the first group, a GLOBAL barrier with the sync/access words and no layout.
    EXPECT(real_list7->barrier_group_count == 2);
    EXPECT(real_list7->barrier_groups[0].Type == D3D12_BARRIER_TYPE_GLOBAL);
    EXPECT(real_list7->barrier_groups[0].NumBarriers == 1);
    EXPECT(real_list7->barrier_globals[0].SyncBefore == D3D12_BARRIER_SYNC_ALL_SHADING);
    EXPECT(real_list7->barrier_globals[0].SyncAfter == D3D12_BARRIER_SYNC_RENDER_TARGET);
    EXPECT(real_list7->barrier_globals[0].AccessBefore == D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
    EXPECT(real_list7->barrier_globals[0].AccessAfter == D3D12_BARRIER_ACCESS_RENDER_TARGET);
    // The real-resource entry is forwarded unchanged as a texture barrier.
    EXPECT(real_list7->barrier_groups[1].Type == D3D12_BARRIER_TYPE_TEXTURE);
    EXPECT(real_list7->barrier_groups[1].NumBarriers == 1);
    EXPECT(real_list7->barrier_textures[0].pResource == console_textures[1].pResource);
    EXPECT(real_list7->barrier_textures[0].SyncBefore == D3D12_BARRIER_SYNC_ALL_SHADING);
    EXPECT(real_list7->barrier_textures[0].LayoutBefore == D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);
    EXPECT(real_list7->barrier_textures[0].LayoutAfter == D3D12_BARRIER_LAYOUT_RENDER_TARGET);
    EXPECT(real_list7->barrier_textures[0].Subresources.IndexOrFirstMipLevel == kAllSubresources);

    // A group whose entries all carry a resource needs no translation: it is forwarded with the console's own pointer.
    D3D12_TEXTURE_BARRIER one_real = console_textures[1];
    D3D12_BARRIER_GROUP real_group{};
    real_group.Type = D3D12_BARRIER_TYPE_TEXTURE;
    real_group.NumBarriers = 1;
    real_group.pTextureBarriers = &one_real;
    raw(list7_proxy, 146, 1, word(&real_group), 0, 0);
    EXPECT(real_list7->calls[80] == 2);
    EXPECT(real_list7->args[80][0] == 1);
    EXPECT(real_list7->args[80][1] == word(&real_group));

    // A group type PC D3D12 has no barrier for, and an unreadable group pointer, are refused: the method is void, so
    // nothing is forwarded and the list cannot be invalidated by the call.
    D3D12_BARRIER_GROUP unknown_group{};
    unknown_group.Type = static_cast<D3D12_BARRIER_TYPE>(7);
    unknown_group.NumBarriers = 1;
    const unsigned forwarded_before = real_list7->calls[80];
    raw(list7_proxy, 146, 1, word(&unknown_group), 0, 0);
    EXPECT(real_list7->calls[80] == forwarded_before);
    raw(list7_proxy, 146, 1, static_cast<std::uint64_t>(0x10), 0, 0);
    EXPECT(real_list7->calls[80] == forwarded_before);

    // 6b. Console slot 52 -> PC slot 48 (ClearRenderTargetView). The retail frame's measured clear passes a handle the
    // proxy's own CreateRenderTargetView filled with a null view (pResource = 0): the console accepts clearing it (it
    // clears nothing), PC D3D12 refuses it and marks the list invalid. The proxy answers the null-view clear as that
    // no-op, forwards a real view unchanged with its CPU handle, and refuses an empty capture no create made.
    palmetto::TargetView null_view{};
    palmetto::TargetView real_view{};
    float clear_colour[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    // CreateRenderTargetView(pResource, pDesc, destination) on the device proxy: the mock writes the layer's capture.
    EXPECT(hr_of(raw(device, 20, 0, 0, word(&null_view), 0)) == S_OK);
    const unsigned clears_before = real_list->calls[48];
    raw(list_proxy, 52, word(&null_view), word(clear_colour), 0, 0);
    EXPECT(real_list->calls[48] == clears_before);  // a null view: refused as the no-op clearing it is
    EXPECT(hr_of(raw(device, 20, 0x7777, 0, word(&real_view), 0)) == S_OK);
    EXPECT(real_view.resource == 0x7777 && real_view.layer_count == 1);
    raw(list_proxy, 52, word(&real_view), word(clear_colour), 0, 0);
    EXPECT(real_list->calls[48] == clears_before + 1);
    EXPECT(real_list->args[48][0] == word(&real_view));
    EXPECT(real_list->args[48][1] == word(clear_colour));
    // A handle no CreateRenderTargetView made: its capture is empty, so it is refused rather than forwarded into a list
    // PC D3D12 would invalidate.
    palmetto::TargetView unknown_view{};
    raw(list_proxy, 52, word(&unknown_view), word(clear_colour), 0, 0);
    EXPECT(real_list->calls[48] == clears_before + 1);
    // A handle created from a real resource whose capture is empty now (something overwrote the descriptor) is refused
    // too: it is not the null view the console's no-op covers.
    palmetto::TargetView emptied_view{};
    EXPECT(hr_of(raw(device, 20, 0x8888, 0, word(&emptied_view), 0)) == S_OK);
    emptied_view = palmetto::TargetView{};
    raw(list_proxy, 52, word(&emptied_view), word(clear_colour), 0, 0);
    EXPECT(real_list->calls[48] == clears_before + 1);

    // ---- Console list slots 70/71/72/74 (docs/bottom_of_pipe.md) ------------------------------------------------
    //
    // Slot 70 is PC slot 62 OMSetDepthBounds with its two floats in XMM1/XMM2: the proxy's float-shaped entry can only
    // forward them with the model on. Slot 71/72 are the measured bottom-of-pipe writes (32 and 64 bits at the title
    // address) and slot 74 is the 64-bit memory wait (comparison 7, GREATER_EQUAL, 64 bits compared).
    {
        using DepthBounds = void(WINAPI*)(void*, float, float);
        auto** const proxy_vtable = *reinterpret_cast<void***>(list7_proxy);
        const xs::ConsoleListExtraCounts before = xs::console_list_extra_counts();

        // The off arm: the console call answers 0, forwards nothing and is not counted by the model.
        xs::set_console_list_extras_enabled(0);
        reinterpret_cast<DepthBounds>(proxy_vtable[70])(list7_proxy, 0.25f, 0.75f);
        EXPECT(real_list7->calls[62] == 0);
        EXPECT(xs::console_list_extra_counts().slot70_calls == before.slot70_calls);

        // The on arm: the same floats reach the real list's PC slot 62 OMSetDepthBounds.
        xs::set_console_list_extras_enabled(1);
        reinterpret_cast<DepthBounds>(proxy_vtable[70])(list7_proxy, 0.25f, 0.75f);
        EXPECT(real_list7->calls[62] == 1);
        EXPECT(real_list7->depth_bounds_calls == 1);
        EXPECT(real_list7->depth_bounds_min == 0.25f && real_list7->depth_bounds_max == 0.75f);
        EXPECT(xs::console_list_extra_counts().slot70_calls == before.slot70_calls + 1);
        EXPECT(xs::console_list_extra_counts().slot70_forwarded == before.slot70_forwarded + 1);

        // Slot 71, Write32BitValueBottomOfPipeX (address, dword, 0, 0): the dword lands in the mapped destination.
        std::uint32_t cell32 = 0;
        const xs::ConsoleListExtraCounts before71 = xs::console_list_extra_counts();
        raw(list7_proxy, 71, word(&cell32), 0xABCD1234ull, 0, 0);
        EXPECT(cell32 == 0xABCD1234u);
        EXPECT(xs::console_list_extra_counts().slot71_calls == before71.slot71_calls + 1);
        EXPECT(xs::console_list_extra_counts().slot71_writes == before71.slot71_writes + 1);
        // A shape outside the measured one (flags 4) is refused by name and writes nothing.
        cell32 = 0;
        raw(list7_proxy, 71, word(&cell32), 0x55, 0, 4);
        EXPECT(cell32 == 0);
        EXPECT(xs::console_list_extra_counts().slot71_refused == before71.slot71_refused + 1);

        // Slot 72, Write64BitValueBottomOfPipeX (address, qword, 0, 0): all 64 bits are written; flags 1 asks for the
        // console's 100 MHz clock instead of the argument value.
        std::uint64_t cell64 = 0;
        const xs::ConsoleListExtraCounts before72 = xs::console_list_extra_counts();
        raw(list7_proxy, 72, word(&cell64), 0x1122334455667788ull, 0, 0);
        EXPECT(cell64 == 0x1122334455667788ull);
        EXPECT(xs::console_list_extra_counts().slot72_writes == before72.slot72_writes + 1);
        EXPECT(xs::console_list_extra_counts().slot72_timestamps == before72.slot72_timestamps);
        cell64 = 0;
        raw(list7_proxy, 72, word(&cell64), 0, 0, 1);
        EXPECT(cell64 != 0);
        EXPECT(xs::console_list_extra_counts().slot72_timestamps == before72.slot72_timestamps + 1);
        // A 64-bit write at an address that is only 4-byte aligned is refused by name.
        cell64 = 0;
        raw(list7_proxy, 72, word(&cell64) + 4, 0x77, 0, 0);
        EXPECT(cell64 == 0);
        EXPECT(xs::console_list_extra_counts().slot72_refused == before72.slot72_refused + 1);

        // Slot 74, Wait64BitValueX (address, comparison 7, qword, flags 1): met, unmet, unreadable and refused.
        std::uint64_t waited = 5;
        const xs::ConsoleListExtraCounts before74 = xs::console_list_extra_counts();
        raw(list7_proxy, 74, word(&waited), 7, 3, 1);
        EXPECT(xs::console_list_extra_counts().slot74_met == before74.slot74_met + 1);
        raw(list7_proxy, 74, word(&waited), 7, 9, 1);
        EXPECT(xs::console_list_extra_counts().slot74_unmet == before74.slot74_unmet + 1);
        raw(list7_proxy, 74, 0x10, 7, 9, 1);  // the null page is not readable title memory
        EXPECT(xs::console_list_extra_counts().slot74_unreadable == before74.slot74_unreadable + 1);
        raw(list7_proxy, 74, word(&waited), 3, 9, 1);  // comparison 3 is not measured
        EXPECT(xs::console_list_extra_counts().slot74_refused == before74.slot74_refused + 1);

        xs::set_console_list_extras_enabled(-1);  // back to the environment switch (off) for anything that follows
    }
    EXPECT(release(list7_proxy) == 0 && real_list7->refs == 0);


    // Clean up
    EXPECT(release(list_proxy) == 0 && real_list->refs == 0);
    EXPECT(release(heap_proxy) == 0);
    EXPECT(release(allocator) == 0 && real_allocator->refs == 0);
    EXPECT(release(queue) == 0 && real_queue->refs == 0);
    EXPECT(release(device) == 0 && mock_device->refs == 0);

    // 7. Verify stats reset
    EXPECT(xs::list_slot_hits(32) > 0);
    xs::reset_device_proxy_stats();
    EXPECT(xs::list_slot_hits(32) == 0);
    EXPECT(!xs::list_slot_first_call_seen(32));

    std::printf("%s: console command list routing, lifecycle, cursor pinning, queue unwrapping, and slot mapping "
                "passed\n", g_label);
    return 0;
}

// ---- The slot-57 record branch: decode -> build -> PC CreateGraphicsPipelineState (docs/graphics_pso.md section 5) ----

// A slot-57 record built by hand (port/src/graphics_record.h): the 0x108 bytes of pointer/size pairs, the two blobs
// the decoder reads (the state record's measured container and the input layout) and the program / kind-2 stage-blob
// bytes whose sizes the decoder checks. This test makes every byte; no game data is involved.
struct Slot57Record {
    std::uint8_t bytes[rt::graphics_record::kDescriptorValidatedBytes] = {};
    // The state record's measured container (docs/state_blob.md): 388 B is the vertex family at k = 1, 464 B the mesh
    // family at k = 0. The decoder proves the container; no fixed-function field of it is used.
    std::uint8_t state[388] = {};
    std::uint8_t mesh_state[464] = {};
    std::uint8_t layout[117] = {};  // the 80-byte empty layout, with room for the one-element layout
    std::uint8_t ps_program[128] = {};
    std::uint8_t vs_program[128] = {};
    std::uint8_t ps_blob[0x30] = {};  // the kind-2 stage blob's measured envelope, 0x10 + one 32-byte record
    std::uint8_t vs_blob[0x30] = {};

    Slot57Record() {
        fill_state_record(state, sizeof(state), true);
        fill_state_record(mesh_state, sizeof(mesh_state), false);
        pair(rt::graphics_record::kStatePairOffset, state, sizeof(state));
        pair(rt::graphics_record::kInputLayoutPairOffset, layout, 0x50);  // the empty layout: element count 0
        pair(rt::graphics_record::kStagePairOffset, ps_program, sizeof(ps_program));  // slot 0: PS
        pair(rt::graphics_record::kStagePairOffset + rt::graphics_record::kProgramToBlobPairOffset, ps_blob,
             sizeof(ps_blob));
        pair(rt::graphics_record::kStagePairOffset + rt::graphics_record::kStagePairStride, vs_program,
             sizeof(vs_program));  // slot 1: VS
        pair(rt::graphics_record::kStagePairOffset + rt::graphics_record::kStagePairStride +
                 rt::graphics_record::kProgramToBlobPairOffset,
             vs_blob, sizeof(vs_blob));
    }

    // One measured state container (docs/state_blob.md): the header words, the variable-region pair and the size
    // family the size names; every fixed-function word stays zero because none is read.
    static void fill_state_record(std::uint8_t* blob, std::size_t size, bool vertex_family) {
        const std::size_t base = vertex_family ? rt::graphics_record::kStateVertexFamilyBaseBytes
                                               : rt::graphics_record::kStateMeshFamilyBaseBytes;
        const std::size_t stage_bytes = vertex_family ? rt::graphics_record::kStateVertexStageBlockBytes
                                                      : rt::graphics_record::kStateMeshStageBlockBytes;
        const std::size_t variable_at =
            rt::graphics_record::kStateHeaderBytes + stage_bytes + rt::graphics_record::kStateCommonBlockBytes;
        const std::size_t items = (size - base) / rt::graphics_record::kStateFamilyStepBytes;
        const std::uint32_t words[5] = {
            rt::graphics_record::kStateHeaderWord00Base | (items != 0 ? 0x08u : 0u),
            vertex_family ? rt::graphics_record::kStateStageWordVertex : rt::graphics_record::kStateStageWordMesh,
            rt::graphics_record::kStateHeaderWord08,
            static_cast<std::uint32_t>(items * 2) | (items != 0 ? 0x20040u : 0u),
            rt::graphics_record::kStateHeaderWord10,
        };
        std::memcpy(blob, words, sizeof words);
        const std::uint32_t pair_word =
            (static_cast<std::uint32_t>(size - variable_at - rt::graphics_record::kStateVariableHeaderBytes) << 16) |
            static_cast<std::uint32_t>(variable_at);
        std::memcpy(blob + rt::graphics_record::kStateVariablePairOffset, &pair_word, sizeof(pair_word));
    }

    void pair(std::size_t at, const void* pointer, std::uint64_t size) {
        const auto address = reinterpret_cast<std::uint64_t>(pointer);
        std::memcpy(bytes + at, &address, sizeof(address));
        std::memcpy(bytes + at + 8, &size, sizeof(size));
    }
    void root(void* signature) {
        const auto value = reinterpret_cast<std::uint64_t>(signature);
        std::memcpy(bytes + rt::graphics_record::kRootSignatureOffset, &value, sizeof(value));
    }
    void layout_size(std::uint64_t size) {
        std::memcpy(bytes + rt::graphics_record::kInputLayoutPairOffset + 8, &size, sizeof(size));
    }
    // A PS+VS record whose input layout is the measured element-table form (count at +0x4C, then per element a u32 name
    // length including the NUL, the name and a 24-byte attribute block, zero-padded to a 4-byte boundary;
    // docs/input_layout.md) with an UNMEASURED format word (7, i.e. not 2/6/16/28/30): the builder refuses it at
    // input_layout.elements[0].format[7], so the slot-57 call returns E_NOTIMPL and the boot histogram lists the value.
    // 0x50 + 4 + 6 ("COLOR") + 24 = 114 bytes, padded to 116, inside this fixture's 117-byte layout buffer.
    void with_element() {
        const std::uint32_t count = 1;
        std::memcpy(layout + rt::graphics_record::kInputLayoutCountOffset, &count, sizeof(count));
        const std::uint32_t name_bytes = 6;  // "COLOR" + NUL
        std::memcpy(layout + rt::graphics_record::kInputLayoutHeaderBytes, &name_bytes, sizeof(name_bytes));
        std::memcpy(layout + rt::graphics_record::kInputLayoutHeaderBytes +
                        rt::graphics_record::kInputLayoutNameLengthBytes,
                    "COLOR", name_bytes);
        const std::size_t attribute = rt::graphics_record::kInputLayoutHeaderBytes +
                                      rt::graphics_record::kInputLayoutNameLengthBytes + name_bytes;
        const std::uint32_t unmeasured_format = 7;
        std::memcpy(layout + attribute + 4, &unmeasured_format, sizeof(unmeasured_format));
        layout_size(116);
    }
    // Slot 2 only (a mesh stage): a D3D12_GRAPHICS_PIPELINE_STATE_DESC has no mesh-shader field. The state record
    // carries the mesh family, because the state size names the record's programmable stage (docs/state_blob.md).
    void mesh_only() {
        pair(rt::graphics_record::kStatePairOffset, mesh_state, sizeof(mesh_state));
        std::memset(bytes + rt::graphics_record::kStagePairOffset, 0, 2 * rt::graphics_record::kStagePairStride);
        pair(rt::graphics_record::kStagePairOffset + 2 * rt::graphics_record::kStagePairStride, ps_program,
             sizeof(ps_program));
        pair(rt::graphics_record::kStagePairOffset + 2 * rt::graphics_record::kStagePairStride +
                 rt::graphics_record::kProgramToBlobPairOffset,
             ps_blob, sizeof(ps_blob));
    }
    // A state pair with no size: outside the decoder's measured envelope, so the record does not decode at all.
    void break_state() { std::memset(bytes + rt::graphics_record::kStatePairOffset + 8, 0, sizeof(std::uint64_t)); }
};

int check_slot57_graphics_record() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 11, true);
    EXPECT(device != nullptr);

    // A root signature created through the proxy is remembered, and a record's +0x00 is passed on only after that: the
    // builder keys the PC pipeline on pRootSignature, so an unproved pointer would be called through by the layer.
    const IID root_iid = __uuidof(ID3D12RootSignature);
    void* root_signature = nullptr;
    std::uint8_t root_blob[64] = {};
    EXPECT(hr_of(raw(device, 16, 1, word(root_blob), sizeof(root_blob), word(&root_iid), word(&root_signature))) == S_OK);
    EXPECT(root_signature != nullptr && mock->calls[16] == 1);
    EXPECT(reinterpret_cast<Mock*>(root_signature)->kind == Kind::root_signature);

    const IID pso_iid = __uuidof(ID3D12PipelineState);
    Slot57Record record;
    record.root(root_signature);
    void* pso = nullptr;

    // 1. A record that decodes and builds. The description the proxy hands the PC device carries the record's two
    //    native programs in their slots, its root signature, the logged attachment stand-in, the documented default
    //    state and an empty input layout.
    EXPECT(hr_of(raw(device, 57, word(&record), word(&pso_iid), word(&pso))) == S_OK);
    const unsigned create_calls = mock->calls[10];
    EXPECT(create_calls == 1 && pso != nullptr);
    Mock* const pso_mock = reinterpret_cast<Mock*>(pso);
    EXPECT(pso_mock->kind == Kind::pipeline_state);
    // The forwarding shape: the title's IID and out pointer reached the PC method (the description is the proxy's own
    // copy, which the mock took by value).
    EXPECT(mock->args[10][1] == word(&pso_iid) && mock->args[10][2] == word(&pso));
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc = mock->graphics_desc;
    EXPECT(desc.pRootSignature == root_signature);
    EXPECT(desc.PS.pShaderBytecode == record.ps_program && desc.PS.BytecodeLength == sizeof(record.ps_program));
    EXPECT(desc.VS.pShaderBytecode == record.vs_program && desc.VS.BytecodeLength == sizeof(record.vs_program));
    EXPECT(!desc.DS.pShaderBytecode && !desc.HS.pShaderBytecode && !desc.GS.pShaderBytecode);
    EXPECT(desc.StreamOutput.NumEntries == 0 && desc.InputLayout.NumElements == 0 && !desc.InputLayout.pInputElementDescs);
    EXPECT(desc.NumRenderTargets == 1 && desc.RTVFormats[0] == DXGI_FORMAT_R8G8B8A8_UNORM &&
           desc.RTVFormats[1] == DXGI_FORMAT_UNKNOWN && desc.DSVFormat == DXGI_FORMAT_UNKNOWN);
    EXPECT(desc.SampleDesc.Count == 1 && desc.SampleDesc.Quality == 0 && desc.SampleMask == 0xffffffffu);
    EXPECT(desc.PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    EXPECT(desc.RasterizerState.FillMode == D3D12_FILL_MODE_SOLID &&
           desc.RasterizerState.CullMode == D3D12_CULL_MODE_BACK && desc.RasterizerState.DepthClipEnable == TRUE);
    EXPECT(desc.BlendState.RenderTarget[0].BlendEnable == FALSE && desc.DepthStencilState.DepthEnable == FALSE);
    EXPECT(desc.NodeMask == 0 && desc.Flags == D3D12_PIPELINE_STATE_FLAG_NONE);
    EXPECT(release(pso) == 0 && pso_mock->refs == 0);

    // 2. The record may name the root signature through a title-side slot: accepted because the value inside is the PC
    //    object this device handed out (the mapping is proved against the root-signature table, not guessed).
    void* root_holder = root_signature;
    record.root(&root_holder);
    pso = nullptr;
    EXPECT(hr_of(raw(device, 57, word(&record), word(&pso_iid), word(&pso))) == S_OK);
    EXPECT(mock->calls[10] == create_calls + 1 && mock->graphics_desc.pRootSignature == root_signature);
    EXPECT(pso != nullptr && release(pso) == 0);
    record.root(root_signature);

    // 3. A record whose input layout element carries an unmeasured format word is refused by
    //    input_layout.elements[0].format[7] (E_NOTIMPL: the format mapping is the decode this tree has not done for
    //    that word; the measured words 2/6/16/28/30 do build). Nothing reaches the PC device.
    {
        Slot57Record elements;
        elements.root(root_signature);
        elements.with_element();
        pso = nullptr;
        EXPECT(hr_of(raw(device, 57, word(&elements), word(&pso_iid), word(&pso))) == E_NOTIMPL);
        EXPECT(pso == nullptr && mock->calls[10] == create_calls + 1);
    }

    // 4. A mesh stage cannot be expressed in a D3D12_GRAPHICS_PIPELINE_STATE_DESC: refused as E_NOTIMPL, nothing sent.
    {
        Slot57Record mesh;
        mesh.root(root_signature);
        mesh.mesh_only();
        pso = nullptr;
        EXPECT(hr_of(raw(device, 57, word(&mesh), word(&pso_iid), word(&pso))) == E_NOTIMPL);
        EXPECT(pso == nullptr && mock->calls[10] == create_calls + 1);
    }

    // 5. A record with no root signature is refused by name (record.root_signature) before the caller's inputs are
    //    reached; an untracked non-null pointer is refused as pRootSignature rather than passed to the layer.
    {
        Slot57Record no_root;
        pso = nullptr;
        EXPECT(hr_of(raw(device, 57, word(&no_root), word(&pso_iid), word(&pso))) == E_INVALIDARG);
        std::uint64_t untracked = 0;
        no_root.root(&untracked);
        EXPECT(hr_of(raw(device, 57, word(&no_root), word(&pso_iid), word(&pso))) == E_INVALIDARG);
        EXPECT(pso == nullptr && mock->calls[10] == create_calls + 1);
    }

    // 6. A record that does not decode is refused before the builder: the title's description is outside the measured
    //    shape, and no input is invented for it.
    {
        Slot57Record broken;
        broken.root(root_signature);
        broken.break_state();
        pso = nullptr;
        EXPECT(hr_of(raw(device, 57, word(&broken), word(&pso_iid), word(&pso))) == E_INVALIDARG);
        EXPECT(pso == nullptr && mock->calls[10] == create_calls + 1);
    }

    // 7. A failed PC call comes back unchanged, with no object: the branch forwards the real HRESULT, it does not
    //    answer success for a pipeline the device refused.
    mock->create_result = E_FAIL;
    pso = nullptr;
    EXPECT(hr_of(raw(device, 57, word(&record), word(&pso_iid), word(&pso))) == E_FAIL);
    EXPECT(pso == nullptr && mock->calls[10] == create_calls + 2);
    mock->create_result = S_OK;

    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: the slot-57 record branch decodes through the real graphics_record/graphics_pso layer, sends the "
                "built description to PC CreateGraphicsPipelineState with the title's IID and out pointer, passes the "
                "record's root signature only when this device handed it out, and refuses an element input layout, a "
                "mesh stage, a missing or untracked root signature and a malformed record by name\n", g_label);
    return 0;
}

// ---- The slot-55 branch: console CreateDerivedPipelineStateX over a slot-57 base (docs/device_slot55.md) ----

// The console patch record: a u32 part word and the part's payload, one 0x14C-byte record per part (the console's
// measured layout; the largest payload is D3D12_BLEND_DESC's 0x148 bytes). The payloads are the PC structures.
struct Slot55Patch {
    std::uint32_t part;
    std::uint8_t payload[0x148];
};
static_assert(sizeof(Slot55Patch) == 0x14C, "the console patch record is 0x14C bytes");

int check_slot55_derived_pipeline_state() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 11, true);
    EXPECT(device != nullptr);

    // The base is a graphics pipeline this device created through console slot 57 (the record branch above). The console
    // derives from that object, so the proxy has to remember the description slot 57 built.
    const IID root_iid = __uuidof(ID3D12RootSignature);
    void* root_signature = nullptr;
    std::uint8_t root_blob[64] = {};
    EXPECT(hr_of(raw(device, 16, 1, word(root_blob), sizeof(root_blob), word(&root_iid), word(&root_signature))) == S_OK);
    Slot57Record record;
    record.root(root_signature);
    const IID pso_iid = __uuidof(ID3D12PipelineState);
    void* base = nullptr;
    EXPECT(hr_of(raw(device, 57, word(&record), word(&pso_iid), word(&base))) == S_OK && base != nullptr);
    const unsigned base_create_calls = mock->calls[10];
    EXPECT(base_create_calls == 1 && mock->calls[55] == 0);

    // A rasterizer (part 1) and a sample mask (part 3): non-zero in every field the check reads, so the derived
    // description cannot match the base's documented default state by accident.
    Slot55Patch patches[2] = {};
    patches[0].part = 1;
    D3D12_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D12_FILL_MODE_WIREFRAME;
    rasterizer.CullMode = D3D12_CULL_MODE_NONE;
    rasterizer.DepthClipEnable = FALSE;
    rasterizer.ForcedSampleCount = 2;
    std::memcpy(patches[0].payload, &rasterizer, sizeof(rasterizer));
    patches[1].part = 3;
    const std::uint32_t sample_mask = 0x0000ffffu;
    std::memcpy(patches[1].payload, &sample_mask, sizeof(sample_mask));

    // 1. The console call reaches the PC device's own CreateGraphicsPipelineState with the base's description rebuilt
    //    and the patched parts replaced; the title's riid and out pointer are the ones forwarded, and the PC object comes
    //    back through them. The console slot itself is answered by the proxy - it never reaches PC slot 55
    //    (CreateReservedResource1), which is what used to fail every one of these calls.
    void* derived = nullptr;
    EXPECT(hr_of(raw(device, 55, word(base), 2, word(patches), word(&pso_iid), word(&derived))) == S_OK);
    EXPECT(mock->calls[55] == 0);
    EXPECT(derived != nullptr && derived != base && mock->calls[10] == base_create_calls + 1);
    EXPECT(reinterpret_cast<Mock*>(derived)->kind == Kind::pipeline_state);
    EXPECT(mock->args[10][1] == word(&pso_iid) && mock->args[10][2] == word(&derived));
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc = mock->graphics_desc;
    EXPECT(desc.pRootSignature == root_signature);
    EXPECT(desc.RasterizerState.FillMode == D3D12_FILL_MODE_WIREFRAME);
    EXPECT(desc.RasterizerState.CullMode == D3D12_CULL_MODE_NONE);
    EXPECT(desc.RasterizerState.DepthClipEnable == FALSE);
    EXPECT(desc.RasterizerState.ForcedSampleCount == 2);
    EXPECT(desc.SampleMask == sample_mask);
    // The parts the call does not patch stay the base's: slot 57's documented default state and attachment stand-in.
    EXPECT(desc.DepthStencilState.DepthEnable == FALSE);
    EXPECT(desc.BlendState.RenderTarget[0].BlendEnable == FALSE);
    EXPECT(desc.NumRenderTargets == 1 && desc.RTVFormats[0] == DXGI_FORMAT_R8G8B8A8_UNORM &&
           desc.DSVFormat == DXGI_FORMAT_UNKNOWN);
    // The stage bytecode is rebuilt from the copy the description table owns: same bytes, not the record's pointer.
    EXPECT(desc.PS.BytecodeLength == sizeof(record.ps_program) &&
           std::memcmp(desc.PS.pShaderBytecode, record.ps_program, sizeof(record.ps_program)) == 0);
    EXPECT(desc.VS.BytecodeLength == sizeof(record.vs_program) &&
           std::memcmp(desc.VS.pShaderBytecode, record.vs_program, sizeof(record.vs_program)) == 0);

    // 2. A pipeline derived from a derived one: the new pipeline state is remembered like a slot-57 one, so a chain of
    //    derivations works and each step keeps the state the previous steps patched.
    Slot55Patch blend_patch = {};
    blend_patch.part = 2;
    D3D12_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
    blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED;
    std::memcpy(blend_patch.payload, &blend, sizeof(blend));
    void* second = nullptr;
    const unsigned after_first = mock->calls[10];
    EXPECT(hr_of(raw(device, 55, word(derived), 1, word(&blend_patch), word(&pso_iid), word(&second))) == S_OK);
    EXPECT(second != nullptr && second != derived && mock->calls[10] == after_first + 1);
    EXPECT(mock->graphics_desc.BlendState.RenderTarget[0].BlendEnable == TRUE);
    EXPECT(mock->graphics_desc.BlendState.RenderTarget[0].SrcBlend == D3D12_BLEND_SRC_ALPHA);
    EXPECT(mock->graphics_desc.BlendState.RenderTarget[0].RenderTargetWriteMask == D3D12_COLOR_WRITE_ENABLE_RED);
    EXPECT(mock->graphics_desc.RasterizerState.FillMode == D3D12_FILL_MODE_WIREFRAME);  // from the first derivation
    EXPECT(mock->graphics_desc.SampleMask == sample_mask);
    EXPECT(release(second) == 0);

    // 3. A base this device did not create has no description to rebuild from: refused by name (E_NOTIMPL) before any
    //    PC call, with the output cleared - not forwarded to a PC method with unrelated arguments.
    {
        void* untracked = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1234));
        void* out = reinterpret_cast<void*>(1);
        const unsigned before = mock->calls[10];
        EXPECT(hr_of(raw(device, 55, word(untracked), 0, 0, word(&pso_iid), word(&out))) == E_NOTIMPL);
        EXPECT(out == nullptr && mock->calls[10] == before && mock->calls[55] == 0);
    }

    // 4. A patch part the console's table does not name has no PC field: the derivation is refused (E_NOTIMPL) rather
    //    than created with unproven state missing, and nothing reaches the PC device.
    {
        Slot55Patch unknown = {};
        unknown.part = 4;
        void* out = reinterpret_cast<void*>(1);
        const unsigned before = mock->calls[10];
        EXPECT(hr_of(raw(device, 55, word(base), 1, word(&unknown), word(&pso_iid), word(&out))) == E_NOTIMPL);
        EXPECT(out == nullptr && mock->calls[10] == before && mock->calls[55] == 0);
    }

    // 5. A null base is an invalid argument (E_INVALIDARG), as the console's first parameter is required.
    {
        void* out = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device, 55, 0, 0, 0, word(&pso_iid), word(&out))) == E_INVALIDARG);
        EXPECT(out == nullptr && mock->calls[55] == 0);
    }

    // 6. Count 0 derives a pipeline that is the base's description unchanged: the console call is legal without patches.
    {
        void* clone = nullptr;
        const unsigned before = mock->calls[10];
        EXPECT(hr_of(raw(device, 55, word(base), 0, 0, word(&pso_iid), word(&clone))) == S_OK);
        EXPECT(clone != nullptr && mock->calls[10] == before + 1);
        EXPECT(mock->graphics_desc.RasterizerState.FillMode == D3D12_FILL_MODE_SOLID);
        EXPECT(mock->graphics_desc.SampleMask == 0xffffffffu);
        EXPECT(release(clone) == 0);
    }

    // 7. A PC refusal comes back unchanged with no object: the branch forwards the host's HRESULT, it does not answer
    //    success for a pipeline the device did not create.
    {
        mock->create_result = E_FAIL;
        void* out = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device, 55, word(base), 0, 0, word(&pso_iid), word(&out))) == E_FAIL);
        EXPECT(out == nullptr);
        mock->create_result = S_OK;
    }

    EXPECT(xs::device_slot_overridden(55));
    EXPECT(release(derived) == 0);
    EXPECT(release(base) == 0);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: the slot-55 branch answers console CreateDerivedPipelineStateX over a slot-57 base - the base's "
                "description is rebuilt with the patched rasterizer/blend/sample-mask parts and sent to PC "
                "CreateGraphicsPipelineState with the title's IID and out pointer, no call reaches PC slot 55, derived "
                "pipelines are tracked for chains, and an untracked base, an unnamed patch part and a null base are "
                "refused by name\n", g_label);
    return 0;
}

// ---- 13. Console slot 70: the measured descriptor publication (docs/device_slot70.md) ----
//
// Measured shape: (arg1 = D3D12_RESOURCE_DESC* of a BUFFER, arg2 = a 0x40-byte record, arg3 = a 32-byte-aligned
// descriptor slot that holds zeros before the call), followed by a cache-line writeback of arg3. Two opt-in behaviours
// are covered here: PALMETTO_SLOT70_PUBLISH=1 writes the MATRIX V# over the record's CPU block (the measured producer of
// the matrix descriptor), and PALMETTO_SLOT70_CBV=1 is the falsified constant-buffer-view conversion. Both switches are
// read once, on the first slot-70 call, so main sets them before any check runs; the default refusal is exercised by the
// --env-slots-invalid run, which the runner starts with no slot-70 switch set.
int check_slot70_console_view() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 11, true);
    EXPECT(device);
    xs::reset_device_proxy_stats();

    // The measured argument contents: a 128-byte BUFFER description, a 0x40-byte record (its first words a CBV desc for
    // the falsified conversion case), and a fresh 32-byte descriptor slot.
    D3D12_RESOURCE_DESC resource{};
    resource.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource.Width = 0x80;
    resource.Height = 1;
    resource.DepthOrArraySize = 1;
    resource.MipLevels = 1;
    alignas(8) std::uint8_t record[0x40] = {};
    D3D12_CONSTANT_BUFFER_VIEW_DESC* view = reinterpret_cast<D3D12_CONSTANT_BUFFER_VIEW_DESC*>(record);
    view->BufferLocation = 0x100000027;
    view->SizeInBytes = 0x1688;
    alignas(32) std::uint32_t destination[8] = {};

    // 1. The falsified conversion still runs when asked: the PC CreateConstantBufferView gets the measured view
    //    description and the title's destination, its descriptor write lands in that slot (the record's block is null,
    //    so the matrix publication does not claim the call).
    {
        const unsigned before = mock->cbv_calls;
        EXPECT(hr_of(raw(device, 70, word(&resource), word(record), word(destination))) == S_OK);
        EXPECT(mock->cbv_calls == before + 1 && mock->calls[70] == 0);
        EXPECT(mock->cbv_dest == word(destination));
        EXPECT(mock->cbv_desc.BufferLocation == 0x100000027 && mock->cbv_desc.SizeInBytes == 0x1688);
        EXPECT(destination[0] == 0x17u && destination[1] == 0x1688u && destination[2] == 0x1717u);
        EXPECT(xs::device_slot_hits(70) == 1);
    }

    // 2. A call outside the measured shape is refused, not written: a null view address, a destination that is not
    //    32-byte aligned, and a description that is not a buffer description. These are the cases where a guess would
    //    write a descriptor over the title's memory.
    {
        const unsigned before = mock->cbv_calls;
        std::uint8_t null_record[0x40] = {};
        EXPECT(hr_of(raw(device, 70, word(&resource), word(null_record), word(destination))) == E_NOTIMPL);
        EXPECT(mock->cbv_calls == before);
        auto* unaligned = reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(destination) + 4);
        EXPECT(hr_of(raw(device, 70, word(&resource), word(record), word(unaligned))) == E_NOTIMPL);
        EXPECT(mock->cbv_calls == before);
        D3D12_RESOURCE_DESC texture{};
        texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture.Width = 0x80;
        EXPECT(hr_of(raw(device, 70, word(&texture), word(record), word(destination))) == E_NOTIMPL);
        EXPECT(mock->cbv_calls == before);
    }

    // 3. The matrix publication (PALMETTO_SLOT70_PUBLISH): the record's CPU block is resolved against the buffer ranges
    //    the title placed through console slot 133, and a MATRIX V# is written over it. The test places a real buffer
    //    (this process's memory) through slot 133, so the block's bytes are readable and its VA is in a placed range.
    {
        static xs::PlacedAtVaHandler placed = [](ID3D12Device*, void*, const D3D12_RESOURCE_DESC&, std::uint32_t,
                                                 REFIID, void** out) -> HRESULT {
            *out = reinterpret_cast<void*>(0x70b10c);
            return S_OK;
        };
        xs::set_placed_at_va_handler(placed);
        alignas(64) static std::uint32_t block_pages[0x1000 / sizeof(std::uint32_t)] = {};  // 4 KiB, first words filled
        for (unsigned i = 0; i < 32; ++i) block_pages[i] = 0x3f800000u + i;  // identity-shaped floats (not title data)
        D3D12_RESOURCE_DESC placed_desc{};
        placed_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        placed_desc.Width = sizeof(block_pages);
        placed_desc.Height = placed_desc.DepthOrArraySize = placed_desc.MipLevels = 1;
        placed_desc.SampleDesc.Count = 1;
        const GUID resource_iid = __uuidof(ID3D12Resource);
        void* placed_out = nullptr;
        EXPECT(hr_of(raw(device, 133, word(block_pages), word(&placed_desc), 0xFFFFFFFFull, 0,
                         word(&resource_iid), word(&placed_out))) == S_OK);

        // The measured record fields: count 0x20 at +0x18, the filled block at +0x28, a declared size at +0x08 and the
        // block's format word at +0x30. The destination is already zeroed (checked by the boot at 12,214 of 12,288
        // calls), so the V# write is the only change.
        std::uint8_t matrix_record[0x40] = {};
        const std::uint64_t block = reinterpret_cast<std::uint64_t>(block_pages);  // inside the placed range
        std::memcpy(matrix_record + 0x08, &view->SizeInBytes, sizeof(std::uint32_t));      // declared = 0x1688
        const std::uint32_t count = 0x20;
        std::memcpy(matrix_record + 0x18, &count, sizeof(count));
        std::memcpy(matrix_record + 0x28, &block, sizeof(block));
        const std::uint32_t format_word = 0x16;
        std::memcpy(matrix_record + 0x30, &format_word, sizeof(format_word));
        alignas(32) std::uint32_t published_dest[8] = {};
        const unsigned cbv_before = mock->cbv_calls;
        EXPECT(hr_of(raw(device, 70, word(&resource), word(matrix_record), word(published_dest))) == S_OK);
        const std::uint32_t expected_lo = static_cast<std::uint32_t>(block & 0xFFFFFFFFu);
        const std::uint32_t expected_hi = static_cast<std::uint32_t>((block >> 32) & 0xFFFFu);
        EXPECT(published_dest[0] == expected_lo && published_dest[1] == expected_hi);
        EXPECT(published_dest[2] == 0x1688u && published_dest[3] == 0x30000FACu);
        EXPECT(published_dest[4] == 0 && published_dest[5] == 0 && published_dest[6] == 0 && published_dest[7] == 0);
        EXPECT(mock->cbv_calls == cbv_before);  // the publication is not the falsified CBV conversion
        EXPECT(mock->calls[70] == 0);           // and the console slot is still never forwarded by number

        // A block whose first 64 bytes are zero is not published: the record is carrying no data yet (logged once).
        const std::uint64_t empty_block = reinterpret_cast<std::uint64_t>(block_pages) + 0x800;  // zeroed region
        std::memcpy(matrix_record + 0x28, &empty_block, sizeof(empty_block));
        alignas(32) std::uint32_t empty_dest[8] = {};
        EXPECT(hr_of(raw(device, 70, word(&resource), word(matrix_record), word(empty_dest))) == E_NOTIMPL);
        EXPECT(empty_dest[0] == 0 && empty_dest[1] == 0);

        // A block outside every placed range is refused too: its V# base would neither be rebased nor pulled.
        static std::uint32_t unplaced_block[16] = {1, 2, 3};
        const std::uint64_t unplaced_block_va = reinterpret_cast<std::uint64_t>(unplaced_block);
        std::memcpy(matrix_record + 0x28, &unplaced_block_va, sizeof(unplaced_block_va));
        EXPECT(hr_of(raw(device, 70, word(&resource), word(matrix_record), word(published_dest))) == E_NOTIMPL);

        // A record that is not the measured shape (element count != 0x20) is not published.
        const std::uint32_t other_count = 0x10;
        std::memcpy(matrix_record + 0x18, &other_count, sizeof(other_count));
        std::memcpy(matrix_record + 0x28, &block, sizeof(block));
        EXPECT(hr_of(raw(device, 70, word(&resource), word(matrix_record), word(published_dest))) == E_NOTIMPL);

        // A texture-shaped publication pairs with the console resource the title created just before it: the
        // publication's resource description matches the slot-127 creation on dimension/width/height/format/mips/array,
        // and the pairing then hands that resource to the real CreateShaderResourceView with a view description built
        // from the publication - the layer's own T#, not a guessed console encoding. A publication whose description does
        // not match any creation keeps the measured refusal.
        {
            const GUID resource_iid = __uuidof(ID3D12Resource);
            D3D12_HEAP_PROPERTIES heap_props{};
            heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC created_desc{};
            created_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            created_desc.Width = 0x100;
            created_desc.Height = 0x80;
            created_desc.DepthOrArraySize = 1;
            created_desc.MipLevels = 1;
            created_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            created_desc.SampleDesc.Count = 1;
            void* created = nullptr;
            EXPECT(hr_of(raw(device, 127, word(&heap_props), 0, word(&created_desc), 0, 0, 0, 0, 0, word(&resource_iid),
                             word(&created))) == S_OK);

            // The publication carries the same resource description and a record at the measured field offsets.
            std::uint8_t texture_record[0x40] = {};
            const std::uint32_t declared = 0x1688;
            std::memcpy(texture_record + 0x08, &declared, sizeof(declared));
            alignas(32) std::uint32_t srv_dest[8] = {};
            const unsigned srv_before = mock->srv_calls;
            EXPECT(hr_of(raw(device, 70, word(&created_desc), word(texture_record), word(srv_dest))) == S_OK);
            EXPECT(mock->srv_calls == srv_before + 1);
            EXPECT(mock->srv_resource == reinterpret_cast<std::uint64_t>(created));
            EXPECT(mock->srv_dest == word(srv_dest));
            EXPECT(mock->srv_desc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D);
            EXPECT(mock->srv_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM);
            EXPECT(mock->srv_desc.Texture2D.MipLevels == 1);
            EXPECT(srv_dest[0] == 0x18u && srv_dest[1] == static_cast<std::uint32_t>(D3D12_SRV_DIMENSION_TEXTURE2D));

            // A description that matches no creation is not published: dimension/format differ from the creation above.
            D3D12_RESOURCE_DESC unmatched_desc = created_desc;
            unmatched_desc.Width = 0x200;
            alignas(32) std::uint32_t unmatched_dest[8] = {};
            EXPECT(hr_of(raw(device, 70, word(&unmatched_desc), word(texture_record), word(unmatched_dest))) == E_NOTIMPL);
            EXPECT(mock->srv_calls == srv_before + 1);
            EXPECT(unmatched_dest[0] == 0 && unmatched_dest[1] == 0);
        }

        // The registry is a lifetime record, not a small window: a creation recorded long before the publication still
        // pairs. One texture is created, many slot-70 calls happen, and only then is its publication made - under a
        // 24-call window this would not pair, with the lifetime registry it does.
        {
            const GUID resource_iid = __uuidof(ID3D12Resource);
            D3D12_HEAP_PROPERTIES heap_props{};
            heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC created_desc{};
            created_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            created_desc.Width = 0x40;
            created_desc.Height = 0x40;
            created_desc.DepthOrArraySize = 1;
            created_desc.MipLevels = 1;
            created_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            created_desc.SampleDesc.Count = 1;
            void* created = nullptr;
            EXPECT(hr_of(raw(device, 127, word(&heap_props), 0, word(&created_desc), 0, 0, 0, 0, 0, word(&resource_iid),
                             word(&created))) == S_OK);
            // A publication that matches no creation: 40 of these run before the real publication (each keeps the
            // measured refusal, and each is a device call between the creation and the pairing).
            D3D12_RESOURCE_DESC other_desc = created_desc;
            other_desc.Width = 0x80;
            alignas(32) std::uint32_t lifetime_dest[8] = {};
            std::uint8_t other_record[0x40] = {};
            for (unsigned i = 0; i < 40; ++i)
                EXPECT(hr_of(raw(device, 70, word(&other_desc), word(other_record), word(lifetime_dest))) == E_NOTIMPL);
            std::uint8_t texture_record[0x40] = {};
            const std::uint32_t declared = 0x1688;
            std::memcpy(texture_record + 0x08, &declared, sizeof(declared));
            const unsigned srv_before = mock->srv_calls;
            EXPECT(hr_of(raw(device, 70, word(&created_desc), word(texture_record), word(lifetime_dest))) == S_OK);
            EXPECT(mock->srv_calls == srv_before + 1);
            EXPECT(mock->srv_resource == reinterpret_cast<std::uint64_t>(created));
            EXPECT(lifetime_dest[0] == 0x18u);
        }

        // A non-buffer resource description is not the measured buffer publication either: the census measured only
        // BUFFER descriptions at this slot, so a texture-shaped record is refused rather than guessed into a V#/T#.
        {
            const std::uint32_t count_ok = 0x20;
            std::memcpy(matrix_record + 0x18, &count_ok, sizeof(count_ok));
            D3D12_RESOURCE_DESC texture_desc{};
            texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            texture_desc.Width = 0x100;
            texture_desc.Height = 0x100;
            texture_desc.DepthOrArraySize = texture_desc.MipLevels = 1;
            texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            texture_desc.SampleDesc.Count = 1;
            alignas(32) std::uint32_t texture_dest[8] = {};
            EXPECT(hr_of(raw(device, 70, word(&texture_desc), word(matrix_record), word(texture_dest))) == E_NOTIMPL);
            EXPECT(texture_dest[0] == 0 && texture_dest[1] == 0 && texture_dest[2] == 0 && texture_dest[3] == 0);
        }

        xs::set_placed_at_va_handler(nullptr);
    }

    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: console slot 70 covers the measured descriptor publication - PALMETTO_SLOT70_CBV=1 still hands "
                "the PC CreateConstantBufferView the measured view and destination, PALMETTO_SLOT70_PUBLISH=1 writes "
                "the MATRIX V# (base = the placed block, stride 0, declared size, raw 0x30000FAC) over a block inside a "
                "buffer placed through console slot 133, and a block outside the placed ranges, an empty block, a "
                "record whose element count is not 0x20 and a texture-shaped resource description are refused\n", g_label);
    return 0;
}

// ---- Allocator slot 9's callback invocation (docs/allocator_slot9.md) -------------------------------------------
//
// The model invokes the registered (callback, context) pair once per allocator proxy, on its first registration, with
// the callback's measured argument shape: arg3 = the recording pool's class, arg4 = the size, arg6 = the context. The
// synthetic callback below proves the invocation, the arguments, the once-per-proxy rule and the two refusals.
struct AllocatorCallbackProbe {
    unsigned calls = 0;
    std::uint64_t a3 = 0;
    std::uint64_t a4 = 0;
    std::uint64_t a6 = 0;
    std::uint64_t block = 0;
};
AllocatorCallbackProbe g_allocator_callback_probe;

std::uint64_t WINAPI test_allocator_callback(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t a3,
                                             std::uint64_t a4, std::uint64_t, std::uint64_t a6) {
    ++g_allocator_callback_probe.calls;
    g_allocator_callback_probe.a3 = a3;
    g_allocator_callback_probe.a4 = a4;
    g_allocator_callback_probe.a6 = a6;
    return g_allocator_callback_probe.block;
}

int check_allocator_callback_invocation() {
    Mock* mock_device = nullptr;
    void* device = make_device(mock_device, 11, true);
    EXPECT(device != nullptr);
    const IID alloc_iid = __uuidof(ID3D12CommandAllocator);
    // The callback stores its block at context+0x12798, and the model reads that cell through the kernel before and
    // after the call: the context has to be readable process memory.
    static std::uint8_t context[0x12800] = {};
    static std::uint64_t block_storage = 0;
    g_allocator_callback_probe = AllocatorCallbackProbe{};
    g_allocator_callback_probe.block = word(&block_storage);

    xs::set_allocator_callback_invocation_enabled(1);
    const xs::AllocatorCallbackCounts before = xs::allocator_callback_counts();

    void* allocator_a = nullptr;
    EXPECT(hr_of(raw(device, 9, 0, word(&alloc_iid), word(&allocator_a))) == S_OK);
    EXPECT(xs::is_allocator_proxy(allocator_a));
    // The first registration on a fresh allocator proxy invokes the pair once, with the measured shape.
    raw(allocator_a, 9, word(&test_allocator_callback), word(context), 0, 0);
    EXPECT(g_allocator_callback_probe.calls == 1);
    EXPECT(g_allocator_callback_probe.a3 == 0x18);      // the class the title's own pool allocation uses
    EXPECT(g_allocator_callback_probe.a4 == 0x10000);   // one console recording chunk
    EXPECT(g_allocator_callback_probe.a6 == word(context));
    // Later registrations on the same proxy only replace the pair.
    raw(allocator_a, 9, word(&test_allocator_callback), word(context), 0, 0);
    EXPECT(g_allocator_callback_probe.calls == 1);
    const xs::AllocatorCallbackCounts after = xs::allocator_callback_counts();
    EXPECT(after.registrations == before.registrations + 2);
    EXPECT(after.first_registrations == before.first_registrations + 1);
    EXPECT(after.invocations == before.invocations + 1);

    // A callback that returns no memory is refused by name.
    g_allocator_callback_probe.block = 0;
    void* allocator_b = nullptr;
    EXPECT(hr_of(raw(device, 9, 0, word(&alloc_iid), word(&allocator_b))) == S_OK);
    raw(allocator_b, 9, word(&test_allocator_callback), word(context), 0, 0);
    EXPECT(g_allocator_callback_probe.calls == 2);
    EXPECT(xs::allocator_callback_counts().refusals == after.refusals + 1);

    // A context the model cannot read is refused before title code is called.
    g_allocator_callback_probe.calls = 0;
    void* allocator_c = nullptr;
    EXPECT(hr_of(raw(device, 9, 0, word(&alloc_iid), word(&allocator_c))) == S_OK);
    raw(allocator_c, 9, word(&test_allocator_callback), 0x10, 0, 0);
    EXPECT(g_allocator_callback_probe.calls == 0);
    EXPECT(xs::allocator_callback_counts().refusals == after.refusals + 2);

    // The off arm: a fresh proxy records the pair and invokes nothing.
    xs::set_allocator_callback_invocation_enabled(0);
    void* allocator_d = nullptr;
    EXPECT(hr_of(raw(device, 9, 0, word(&alloc_iid), word(&allocator_d))) == S_OK);
    raw(allocator_d, 9, word(&test_allocator_callback), word(context), 0, 0);
    EXPECT(g_allocator_callback_probe.calls == 0);
    xs::set_allocator_callback_invocation_enabled(-1);

    EXPECT(release(allocator_d) == 0 && release(allocator_c) == 0 && release(allocator_b) == 0 &&
           release(allocator_a) == 0);
    EXPECT(release(device) == 0 && mock_device->refs == 0);
    std::printf("%s: allocator slot 9's callback is invoked once per allocator with the measured class/size/context "
                "arguments, a null return and an unreadable context are refused, and the off arm invokes nothing\n",
                g_label);
    return 0;
}

int check_console_texture_translation() {
    // The measured shape of the console's depth-stencil + unordered-access pair: a 2D R16_TYPELESS texture with
    // ALLOW_DEPTH_STENCIL | ALLOW_UNORDERED_ACCESS and the array-layer counts the loading stage asked for.
    const auto measured = [](UINT16 layers) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = desc.Height = 768;
        desc.DepthOrArraySize = layers;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R16_TYPELESS;
        desc.SampleDesc.Count = 1;
        desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        return desc;
    };
    constexpr auto both = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    for (const UINT16 layers : {UINT16(6), UINT16(1), UINT16(3)}) {
        D3D12_RESOURCE_DESC desc = measured(layers);
        EXPECT(palmetto::console_depth_uav_translated(desc));
        EXPECT(desc.Flags == D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        EXPECT(desc.DepthOrArraySize == layers && desc.Format == DXGI_FORMAT_R16_TYPELESS &&
               desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.MipLevels == 1);  // everything else as asked
    }
    // The other depth-stencil families translate the same way: the rule is the family, not only the measured format.
    for (const DXGI_FORMAT format : {DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32G8X24_TYPELESS,
                                     DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_R24G8_TYPELESS,
                                     DXGI_FORMAT_D24_UNORM_S8_UINT}) {
        D3D12_RESOURCE_DESC desc = measured(1);
        desc.Format = format;
        EXPECT(palmetto::console_depth_uav_translated(desc));
        EXPECT(desc.Flags == D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    }
    // Genuinely invalid combinations stay exactly as they were, so the device's own named refusals still fire for them.
    {
        D3D12_RESOURCE_DESC desc = measured(1);
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;  // no unordered access: nothing to translate
        EXPECT(!palmetto::console_depth_uav_translated(desc));
        EXPECT(desc.Flags == D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    }
    {
        D3D12_RESOURCE_DESC desc = measured(1);
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;  // unordered access alone: nothing to translate
        EXPECT(!palmetto::console_depth_uav_translated(desc));
        EXPECT(desc.Flags == D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    }
    {
        D3D12_RESOURCE_DESC desc = measured(1);
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // depth-stencil on a colour texture is refused, not translated
        EXPECT(!palmetto::console_depth_uav_translated(desc));
        EXPECT(desc.Flags == both);
    }
    {
        D3D12_RESOURCE_DESC desc = measured(1);
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;  // a buffer never carries the console pair
        EXPECT(!palmetto::console_depth_uav_translated(desc));
        EXPECT(desc.Flags == both);
    }
    // The family helper itself, shared with the device's validation.
    EXPECT(palmetto::format_family(DXGI_FORMAT_R16_TYPELESS) == DXGI_FORMAT_R16_TYPELESS);
    EXPECT(palmetto::format_family(DXGI_FORMAT_R8G8B8A8_UNORM) == DXGI_FORMAT_R8G8B8A8_TYPELESS);
    EXPECT(palmetto::depth_stencil_family(DXGI_FORMAT_R16_TYPELESS));
    EXPECT(!palmetto::depth_stencil_family(DXGI_FORMAT_R8G8B8A8_UNORM));
    // The console Series-S upscale output shape (docs/decomp_renderer.md section 12): the measured 1920x1080
    // R9G9B9E5_SHAREDEXP render target with unordered access, and the console-type spelling the translation is gated on.
    const auto upscale_output = [] {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = 1920;
        desc.Height = 1080;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
        desc.SampleDesc.Count = 1;
        desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        return desc;
    };
    EXPECT(palmetto::console_r9g9b9e5_render_target(upscale_output()));
    {
        D3D12_RESOURCE_DESC desc = upscale_output();
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;  // a render target without unordered access
        EXPECT(!palmetto::console_r9g9b9e5_render_target(desc));
    }
    {
        D3D12_RESOURCE_DESC desc = upscale_output();
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;  // unordered access alone is the existing UAV emulation
        EXPECT(!palmetto::console_r9g9b9e5_render_target(desc));
    }
    {
        D3D12_RESOURCE_DESC desc = upscale_output();
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // the same flags on another format are not this shape
        EXPECT(!palmetto::console_r9g9b9e5_render_target(desc));
    }
    {
        D3D12_RESOURCE_DESC desc = upscale_output();
        desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(UINT(desc.Flags) | UINT(D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL));
        EXPECT(!palmetto::console_r9g9b9e5_render_target(desc));  // depth-stencil stays refused by the format rules
    }
    {
        D3D12_RESOURCE_DESC desc = upscale_output();
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;  // a volume has no attachment of this shape
        EXPECT(!palmetto::console_r9g9b9e5_render_target(desc));
    }
    {
        D3D12_RESOURCE_DESC desc = upscale_output();
        desc.SampleDesc.Count = 2;  // measured single-sample; multisampled stays unmeasured
        EXPECT(!palmetto::console_r9g9b9e5_render_target(desc));
    }
    {
        D3D12_RESOURCE_DESC desc = upscale_output();
        desc.MipLevels = 2;
        EXPECT(!palmetto::console_r9g9b9e5_render_target(desc));
    }
    // The console-type gate: only the Series-S spellings the loader accepts match; the Series X/devkit values, the
    // default-path spellings and anything malformed leave the translation off.
    for (const char* series_s : {"6", "seriess", "series-s", "series s", " 6 ", "SeriesS", "SERIES-S"})
        EXPECT(palmetto::console_type_is_series_s(series_s));
    for (const char* not_series_s :
         {static_cast<const char*>(nullptr), "", "0", "off", "1", "pc", "7", "seriesx", "series-x", "8", "devkit",
          "06", "6x", "series"})
        EXPECT(!palmetto::console_type_is_series_s(not_series_s));
    std::printf("%s: the console's depth-stencil + unordered-access pair is translated to the depth-stencil resource "
                "for the measured R16_TYPELESS 6/1/3-layer shapes and the other depth families, while a colour "
                "texture, a buffer and either flag alone stay unchanged; the measured Series-S R9G9B9E5 upscale "
                "output is the only shape the console render-target rule accepts, behind the Series-S console-type "
                "spellings\n",
                g_label);
    return 0;
}

int main(int argc, char** argv) {
    no_dialogs();
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // Console fence slot 12's watcher (docs/fence_slot12.md): the GPU-free stand-in for the Vulkan layer, which
    // registers its fence dispatcher here in the runtime. It has to be in place before the fence checks call slot 12.
    ::palmetto_register_fence_value_watcher(
        reinterpret_cast<void*>(static_cast<xs::FenceValueWatcherFn>(&test_fence_watcher)));
    if (argc == 2) {
        if (std::strcmp(argv[1], "--env-slots") == 0) return run_env_slots(true);
        if (std::strcmp(argv[1], "--env-slots-invalid") == 0) return run_env_slots(false);
        return run_fault_mode(argv[1]);
    }
    if (argc != 1) return 2;
    // Console slot 70's measurement switches are read once, so the conversion case has to be on before this process's
    // first slot-70 call (check_slot70_console_view below). The default refusal is the --env-slots-invalid run, which
    // the runner starts with no slot-70 switch set.
    EXPECT(::SetEnvironmentVariableA("PALMETTO_SLOT70_CBV", "1") != FALSE);
    EXPECT(::SetEnvironmentVariableA("PALMETTO_SLOT70_PUBLISH", "1") != FALSE);
    EXPECT(::SetEnvironmentVariableA("PALMETTO_SLOT70_CENSUS", "1") != FALSE);
    EXPECT(::SetEnvironmentVariableA("PALMETTO_SLOT70_PAIR", "1") != FALSE);
    if (check_tail(true) || check_queue_console_slots() || check_queue_copy_page_mappings() || check_levels() ||
        check_fence1() || check_create_failures() || check_open_shared() || check_alloc_failures() ||
        check_allocators() || check_xbox_slots() || check_frame_event_forwarding() ||
        check_slot74_and_slot25_standin() ||
        check_console_command_list() || check_allocator_callback_invocation() || check_slot57_graphics_record() ||
        check_slot55_derived_pipeline_state() || check_console_command_signature() ||
        check_console_texture_translation() || check_slot70_console_view())
        return 1;
    std::printf("xs_proxy_mock_test: PASS (%d allocations refused on purpose)\n", g_failed_news);
    return 0;
}
