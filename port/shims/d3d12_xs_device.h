// Xbox-ABI proxy around a PC ID3D12Device.
//
// Measured Xbox device deviations used here (vtable indices, x64):
//   slot 41   CreateCommandSignature at its PC position (desc, root signature, iid, out): the retail title's renderer
//             setup creates five one-argument signatures (argument type / ByteStride: DRAW / 16, DRAW_INDEXED / 20,
//             105 / 12, DISPATCH / 12, DISPATCH_MESH / 12). Console type 105, which has no PC value, is created as
//             DISPATCH; the DISPATCH_MESH description is answered with a named dispatch-shaped stand-in because the
//             layer carries no mesh signature, and an ExecuteIndirect naming that stand-in is refused by name.
//             docs/command_signature.md has the call sites, the decode and the limits.
//   slot 62   CreateComputePipelineState(desc, w2, w3, iid, out): two words (0 from XboxBvhBuilder, meaning unknown)
//             before the IID. The PC slot 62 is CreateStateObject.
//   slot 67   CreateComputePipelineState from native code (desc, iid, out): converts the Xbox native-code desc
//             (+0x00 root signature, +0x28 code, +0x30 size) to D3D12_COMPUTE_PIPELINE_STATE_DESC and forwards
//             to PC CreateComputePipelineState (slot 11). The PC slot 67 is CreateProtectedResourceSession1.
//   slot 76   resource creation -> PC CreateCommittedResource (PC slot 27), with the Xbox layout word ignored.
//   slot 48   CreateCommandQueueX(desc, riid, out) -> the same queue proxy as slot 8. The Xbox desc's first fields are
//             D3D12_COMMAND_QUEUE_DESC's (public HLE).
//   slot 73   CreateCommandListX(desc, allocator, initialState, riid, out) -> PC CreateCommandList(0, type, allocator,
//             initialState, riid, out). The type is the first word of the Xbox desc; there is no node-mask argument
//             in the console call (public HLE; the PC six-argument form differs).
//   slots 78/79/80  SetFrameIntervalX, ScheduleFrameEventX, WaitFrameEventX. A per-device token state machine answers
//             them before the PC vtable is indexed; WaitFrameEventX returns at once with a fresh non-zero token.
//             Slot 80 is outside a PC ID3D12Device11 table, so it is checked before the beyond-table refusal.
// The slots 48/73/78/79/80 are the public HLE Freminet/ConsoleX's observation, not Microsoft documentation, and can
// be moved with PALMETTO_XS_QUEUE_X_SLOT, PALMETTO_XS_LIST_X_SLOT, PALMETTO_XS_FRAME_INTERVAL_SLOT,
// PALMETTO_XS_SCHEDULE_EVENT_SLOT and PALMETTO_XS_WAIT_EVENT_SLOT (3..127; anything else is logged once and ignored).
// The measured slots 9/10 (GetCPU/GPUDescriptorHandleForHeapStart) are ID3D12DescriptorHeap methods, not device
// methods: the PC device's 9/10 are CreateCommandAllocator/CreateGraphicsPipelineState and are forwarded unchanged.
// On the PC heap those getters use the hidden return pointer in RDX; the Xbox ABI returns the 8-byte handle in
// RAX. Device slot 14 therefore wraps the created heap in a heap proxy that performs that conversion. The heap
// proxy holds a reference to the device proxy, and its ID3D12DeviceChild::GetDevice returns that same proxy.
// The proxy's heap slots 9/10 expose only the Xbox shape: a PC C++ caller using the PC header's return convention
// would not receive the value, while GetDesc and the other heap slots forward the PC convention unchanged. A
// direct one-argument call against a real PC heap is invalid, so the device test exercises the conversion on the
// wrapper and compares it with a hidden-pointer call against the real heap.
//
// Every other PC device slot is forwarded by a generated thunk that swaps `this` to the PC device. The proxy's
// vtable therefore follows the PC device order, with slots 62 and 76 and the public HLE's 48/73/78/79/80 deliberately
// shadowed. The full Xbox vtable order is not known from the spec; slots beyond the PC device's own vtable return
// E_NOTIMPL and log once.
//
// Command queues and fences (device slots 8, 75, 36, and 32 for a shared fence) come back wrapped in queue and fence
// proxies of the same kind: the public chain (19 queue slots, 11 or 12 fence slots in SDK 10.0.26100) is forwarded
// in PC order, and a slot past it, up to the end of the proxy's table (128 queue, 64 fence slots), is logged with its
// first four argument words and refused with E_NOTIMPL. A slot past the table faults on the vtable read inside a
// reserved, never-committed region, and the fault is logged with the slot. That is where the console's queue methods
// (PresentX, SuspendX, ResumeX) will show up; their slots are not known and are not guessed, and that the console
// keeps the public slots is an assumption (docs/d3d12_command_objects.md §5.2). Heap, queue and fence proxies are
// unwrapped wherever a device or queue method hands them to the real runtime: queue Signal/Wait (14/15), device
// CreateSharedHandle (31), MakeResident/Evict (34/35), SetEventOnMultipleFenceCompletion (45), SetResidencyPriority
// (46) and EnqueueMakeResident (50). Command lists are not wrapped, so ExecuteCommandLists passes them through and a
// heap proxy given to a list's SetDescriptorHeaps reaches the real list as it is. QueryInterface answers only the
// modelled interfaces; one the real object has but the proxy does not model is logged once and refused.
#pragma once

#include <d3d12.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace rt::d3d12_xs {

// Wraps `real` and returns the requested device interface through `out`. Takes ownership of one reference to
// `real` on success; on failure the caller keeps its reference. Returns E_NOINTERFACE if `riid` is not a D3D12
// device interface the PC device implements.
HRESULT create_device_proxy(ID3D12Device* real, REFIID riid, void** out);

// True when `device` points at a proxy created by create_device_proxy. Used by the tests.
bool is_device_proxy(const void* device);

// IDXGIDevice1 on the device. A GDKX title asks its device for IDXGIDevice1, then GetAdapter and EnumOutputs(0),
// to find the output it gives SetFrameIntervalX (docs/d3d12_vulkan.md section 4.5). The proxy answers IDXGIObject,
// IDXGIDevice and IDXGIDevice1 itself, as one COM object with the device (same reference count, QueryInterface
// both ways). GetAdapter and GetParent return the adapter of the device behind the proxy: by default the host DXGI
// adapter with that device's LUID. A provider replaces that: `real` is the device behind the proxy, and it returns
// the adapter as `riid`. rt::d3d12::set_device_outer sets one for the Vulkan-backed device (the layer's own adapter).
//
// A title may ask D3D12XboxCreateDevice (so create_device_proxy) for IDXGIDevice1 directly. What comes back then is
// the proxy's DXGI pointer, not the proxy: is_device_proxy is false for it. Code that takes "the proxy" from a
// creation call goes through device_proxy_primary first.
using DxgiAdapterProvider = HRESULT (*)(ID3D12Device* real, REFIID riid, void** out);
// `device` is the proxy or its DXGI pointer. False, with a line on stderr, when it is neither: nothing is set then.
bool set_device_adapter_provider(void* device, DxgiAdapterProvider provider);
// The device proxy an IDXGIDevice pointer handed out by one belongs to, or null.
void* device_proxy_from_dxgi_device(void* dxgi_device);
// The device proxy itself, given the proxy or its DXGI pointer; null for anything else.
void* device_proxy_primary(void* device);

// True when `heap` is a descriptor-heap proxy created by device slot 14.
bool is_heap_proxy(const void* heap);
ID3D12DescriptorHeap* heap_proxy_real_heap(const void* heap);

// Number of PC vtable slots the proxy forwards (detected from the interfaces the PC device supports).
unsigned device_proxy_slot_count(const void* device);

// The PC device behind a proxy. Null when `device` is not a proxy.
ID3D12Device* device_proxy_real_device(const void* device);

// Device slot 133 (the console's CreatePlacedResourceX) makes a buffer on the title's pages at `va`. The proxy does
// not create it itself: the device behind it does, through this handler, which the Vulkan layer registers
// (rt::d3d12::create_buffer_at_host_address). With none registered, or a PC device behind the proxy, the slot
// answers E_NOTIMPL.
// `layout` is the title's third argument, a D3D12_BARRIER_LAYOUT (0xFFFFFFFF = UNDEFINED), measured on a retail
// boot: 0x6 SHADER_RESOURCE, 0x3 UNORDERED_ACCESS, as PC CreatePlacedResource2 takes it.
using PlacedAtVaHandler = HRESULT (*)(ID3D12Device* real, void* va, const D3D12_RESOURCE_DESC& desc,
                                      std::uint32_t layout, REFIID riid, void** out);
void set_placed_at_va_handler(PlacedAtVaHandler handler);
PlacedAtVaHandler placed_at_va_handler();

// Called by slot 133 for a buffer, before the handler, with the buffer's range [va, va + width). palmetto registers
// rt::xmem::commit_physical_range: xmem commits physical pages on first touch, so a range the title has mapped but
// not written yet is still reserved, and the handler's import refuses reserved pages (it would answer with its
// fallback buffer, which the title's writes never reach). A false return is logged once and the handler is still
// called. With none registered nothing is done.
using PlacedAtVaPagesHook = bool (*)(const void* va, std::size_t bytes);
void set_placed_at_va_pages_hook(PlacedAtVaPagesHook hook);
PlacedAtVaPagesHook placed_at_va_pages_hook();

// Device slot 127 creates the title's two display surfaces. On the console the runtime's allocation of each surface's
// memory runs the title's own exported XMem allocation hook (image+0x6844380, the image's one export), which carves the
// surface from the 64 MiB window scope the title registers around those two creations; the title then checks at
// game+0x247af25 that the window is nearly used up (docs/range_assert.md). The proxy is the runtime for slot 127 here,
// so it makes that call for a display surface before the host creation. `bytes` is the surface's byte size and
// `attributes` is the hook's routing word: bits 23:16 are the hook bucket the title's registration pushes to (0x80),
// and bit 15 set with bit 14 clear is the handler's reserve-and-carve branch (game+0x56a2a60 tests the same mask).
// The default allocator reads the hook table from the mapped image and returns null before a title is loaded (the
// tests); a test can replace it with a recording stub.
using TitleSurfaceAllocator = void* (*)(std::uint64_t bytes, std::uint32_t attributes);
void set_title_surface_allocator(TitleSurfaceAllocator allocator);
TitleSurfaceAllocator title_surface_allocator();

// True for slots the proxy handles itself rather than forwarding verbatim: IUnknown (0-2), 8 and 75 (queue
// proxies), 9 (allocator proxies), 14, 31/34/35/45/46/50 (proxies unwrapped), 32 and 36 (fence proxies), 48/73 and
// 78/79/80 (the Xbox slots above, at their configured indices), 62, 67 and 76. Slot 10 is intentionally false: the
// measured 9/10 handle conversion is on the heap proxy, and device slot 9 is CreateCommandAllocator, which the proxy
// handles for the allocator proxy.
bool device_slot_overridden(unsigned slot);

// True when `signature` is the dispatch-shaped stand-in the proxy created for the console's DISPATCH_MESH
// command-signature description (docs/command_signature.md). The layer carries no mesh command-signature kind, so the
// creation is answered with the stand-in and the list proxy refuses an ExecuteIndirect that names it by name instead of
// recording a dispatch the console would have run as mesh work.
bool command_signature_standin(const void* signature);

// Queue and fence proxies created by device slots 8/75 and 36. The real_* accessors return the wrapped object
// (no reference added), or null when the pointer is not such a proxy.
bool is_queue_proxy(const void* queue);
bool is_fence_proxy(const void* fence);
ID3D12CommandQueue* queue_proxy_real_queue(const void* queue);
ID3D12Fence* fence_proxy_real_fence(const void* fence);
// Public slots forwarded (19 for a queue; 11 or 12 for a fence), and the size of the proxy's vtable.
unsigned queue_proxy_slot_count(const void* queue);
unsigned fence_proxy_slot_count(const void* fence);
unsigned queue_proxy_table_size();
unsigned fence_proxy_table_size();
// ---- Console-order queue calls ---------------------------------------------------------------------------------
//
// The console queue inserts two methods before Signal, so its Signal is console slot 16 and its Wait slot 17; the proxy
// maps those two (and 18/19/20 = timestamp frequency, clock calibration, GetDesc) onto the PC slots two below them
// (docs/d3d12_xs_surface.md; measured on this title: Signal 16, GetDesc 20). Console slots 14 and 15 are the inserted
// methods: their shapes are not identified, so the proxy refuses them by name with E_NOTIMPL. A PC-ABI caller that
// invokes ID3D12CommandQueue::Signal through the Xbox-ABI object therefore lands on console slot 14 and is refused, not
// signalled. These helpers make the call at the console slot the title uses, for a fixture or client holding a queue
// proxy; they return E_INVALIDARG for anything that is not a queue proxy (the proxy's own refusal still logs the slot).
HRESULT queue_proxy_console_signal(void* queue, ID3D12Fence* fence, std::uint64_t value);
HRESULT queue_proxy_console_wait(void* queue, ID3D12Fence* fence, std::uint64_t value);
// ---- Console fence slot 12 (vtable offset 0x60) ----
//
// A fence proxy answers console fence slot 12 with the address of a 64-bit cell that carries the fence's completed
// value. Measured from the title's wait loop (docs/fence_slot12.md): it calls (fence, target, &continuation, flags),
// keeps the result, then polls [result] with monitorx/mwaitx until the word there reaches the target, testing it
// with the title's 56-bit comparison and treating UINT64_MAX as a removed device. The PC ID3D12Fence chain ends at
// 11/12, so this method is a console extension: the console keeps a fence's completed value in CPU-visible memory,
// and this is its address, which is why the title stores its fence as the waitable of its submit channel.
//
// The cell is the proxy's copy of that value. It is set from the real fence when slot 12 is called and when the
// fence is advanced through a proxy, and a target the fence has not reached yet is handed to the Vulkan layer's
// fence dispatcher so GPU completion moves the cell too. This file has no Vulkan dependency (it is also linked into
// the GPU-free test binaries), so the layer registers the watcher at startup; with none registered the cell moves
// only when the fence is advanced through its proxy, and the first such wait says so in the log.
inline constexpr unsigned kConsoleFenceValueAddress = 12;
using FenceValueWatcherFn =
    void (*)(IUnknown* real_fence, const std::shared_ptr<std::atomic<std::uint64_t>>& cell, std::uint64_t target);

// Live references on a queue, fence or allocator proxy (0 for anything else). Test helper.
unsigned long command_object_proxy_ref_count(const void* object);
// Per-slot call counters and first-call flags, shared by all queue proxies and all fence proxies.
std::uint64_t queue_slot_hits(unsigned slot);
bool queue_slot_first_call_seen(unsigned slot);
std::uint64_t fence_slot_hits(unsigned slot);
bool fence_slot_first_call_seen(unsigned slot);
// Vtable reads past a queue or fence proxy table that faulted in its guard region (counted by the fault logger).
std::uint64_t queue_table_overruns();
std::uint64_t fence_table_overruns();
// True when the queue, fence and allocator tables sit in front of their guard regions (false only if a reservation
// or the fault logger failed).
bool command_object_tables_guarded();

// Command allocator proxies (device slot 9): the public PC chain (8 slots, Reset at slot 8) is forwarded. Console
// slot 9 is the allocator-callback registration (docs/allocator_slot9.md): the title calls it (callback, context) on
// the allocator it is about to use - once per presented frame on this title - and ignores the answer; on the console
// that callback is what the runtime would invoke when it backs the allocator's recording memory. Our Vulkan layer owns
// that memory itself, so the proxy records the pair on the proxy (lifetime: the proxy's) and answers S_OK without
// invoking the callback or addressing the real allocator. Slots 10..63 are logged and refused with E_NOTIMPL, and a
// read past the table faults in a guard region, as for queues and fences. Device slot 9 wraps only an allocator that
// answers kPalmettoCommandAllocatorTag, Palmetto's own Vulkan-layer allocator: its command lists resolve the proxy
// (CreateCommandList, Reset). A PC runtime allocator goes back unchanged, because the PC runtime's lists would take a
// proxy for their own allocator; a command-list proxy that unwraps at Reset is what lets the host backend wrap
// allocators too.
// {7d0e6b51-34a2-4f0c-9a61-0c5b2e8f4a10}: the Vulkan layer's CommandAllocator answers it with its implementation.
inline constexpr GUID kPalmettoCommandAllocatorTag{
    0x7d0e6b51, 0x34a2, 0x4f0c, {0x9a, 0x61, 0x0c, 0x5b, 0x2e, 0x8f, 0x4a, 0x10}};
bool is_allocator_proxy(const void* allocator);
// The wrapped allocator (no reference added), or null when `allocator` is not an allocator proxy.
ID3D12CommandAllocator* allocator_proxy_real_allocator(const void* allocator);
unsigned allocator_proxy_slot_count(const void* allocator);
unsigned allocator_proxy_table_size();
// What console allocator slot 9 registered on one proxy: {0, 0} when the slot was never called on this proxy, when the
// last call passed zeros, or when `allocator` is not an allocator proxy. A later call replaces the pair. One title thread
// registers per allocator, so a concurrent registration and read can at worst pair a new callback with the previous
// context; nothing in the runtime reads the pair, it is a record for tests and diagnostics.
struct AllocatorCallback {
    std::uintptr_t callback = 0;
    std::uintptr_t context = 0;
};
AllocatorCallback allocator_proxy_callback(const void* allocator);
std::uint64_t allocator_slot_hits(unsigned slot);
bool allocator_slot_first_call_seen(unsigned slot);
std::uint64_t allocator_table_overruns();
// ---- Console list slots 70/71/72/74: OMSetDepthBounds, the bottom-of-pipe writes and the 64-bit memory wait ------
//
// docs/bottom_of_pipe.md. Measured on this title (RVAs relative to the image base):
//   * console list slot 70 is PC slot 62 OMSetDepthBounds. The only call site is game+0x33390 (return 0x33396) in
//     the recorder game+0x32520..0x33CB5: the list is in RCX and the two floats are in XMM1/XMM2, after a two-float
//     compare and store of the same pair. The generic integer thunk cannot carry the floats, so the proxy's table
//     entry at 70 is a float-shaped function that forwards to the real vtable's PC slot 62.
//   * slot 71 = Write32BitValueBottomOfPipeX(address, dword, flush, flags); measured (address, dword, 0, 0) at
//     game+0x4ED46AA. Its caller game+0x4ED4670 computes the address with game+0x4ED46D0: base[0x75DF268] +
//     frame_counter*72 + index*4 (18 dwords a frame, one per parallel record), read from a field at [owner+0x1F9C4].
//   * slot 72 = Write64BitValueBottomOfPipeX(address, qword, flush, flags); measured (address, 0, 0, 1) at
//     game+0x2482B37: the address is record->+0x58 + index*8 and flags 1 (also 2 on the console) asks for a
//     timestamp instead of the value, written when the list is recorded.
//   * slot 74 = Wait64BitValueX(address, comparison, qword, flags); measured (address, 7, qword, 1) at game+0x2484B12.
//     Comparison 7 is GREATER_EQUAL and the 64 bits are compared; the called record's cell is written on the GPU. The
//     console holds the queue until the cell holds.
//
// The model behind PALMETTO_XS_CONSOLE_LIST_EXTRAS=1 (off by default): slot 70 forwards to PC slot 62, slots 71/72
// write the measured value into the mapped title address at record time (the destination is CPU-visible process
// memory here; the marker-out GPU form would need a replay command, see the page's limits), and slot 74 evaluates the
// cell (met/unmet; a cell the mapped title memory does not let us read is refused by name). Every shape outside the
// measured one is refused by name and counted. PALMETTO_XS_CONSOLE_LIST_TRACE=1 logs the measured shapes without
// changing a call. Counters are per-process and exposed for the tests and the boot's summary line.
inline constexpr unsigned kConsoleOmSetDepthBounds = 70;
inline constexpr unsigned kPcOmSetDepthBounds = 62;
struct ConsoleListExtraCounts {
    std::uint64_t slot70_calls = 0, slot70_forwarded = 0;
    std::uint64_t slot71_calls = 0, slot71_writes = 0, slot71_refused = 0;
    std::uint64_t slot72_calls = 0, slot72_writes = 0, slot72_timestamps = 0, slot72_refused = 0;
    std::uint64_t slot74_calls = 0, slot74_met = 0, slot74_unmet = 0, slot74_unreadable = 0, slot74_refused = 0;
};
ConsoleListExtraCounts console_list_extra_counts();
// 1: on, 0: off, -1: the PALMETTO_XS_CONSOLE_LIST_EXTRAS environment switch (the default). Tests use this to exercise
// both arms in one process; nothing in the runtime sets it.
void set_console_list_extras_enabled(int mode);
bool console_list_extras_enabled();

// ---- The cross-list memory wait and GPU-ordered bottom-of-pipe writes (docs/peer_port_streaming.md section 11) ------
//
// The model above writes slots 71/72 at record time and evaluates slot 74 then, so it never holds. These switches port
// the console's GPU ordering on top of it, all off by default and only meaningful with PALMETTO_XS_CONSOLE_LIST_EXTRAS:
//   PALMETTO_XS_MEMORY_WAIT_HOLD=1     split a list at an unmet wait, hold the segment after it, release it when the
//                                     cell holds (a later list's write, or the watcher's poll) or at the bound;
//   PALMETTO_XS_MEMORY_WAIT_BOUND_MS   the bound, default 5000 (the peers' bound), 1..600000;
//   PALMETTO_XS_MEMORY_WAIT_MAX        concurrent held segments, default 16, 1..256;
//   PALMETTO_XS_BOTTOM_OF_PIPE_GPU=1   write slot 71/72 values as GPU-ordered immediate writes at the recorded position
//                                     (desktop WriteBufferImmediate, MARKER_OUT) instead of at record time;
//   PALMETTO_XS_MEMORY_WAIT_TRACE=1    the first refusals and hold events are logged, no call changes.
// The release is the layer's own Wait-before-Signal hold (queue_holds.h): the shim posts the queue's PC Wait on a
// fence before the held segment and the watcher CPU-signals that fence, which the layer turns into the replay of the
// held batch. A held list is split into real lists, and the state it set before the wait is replayed into the segment
// after it; a list whose state cannot be replayed (render targets, descriptor heaps, predication, an open query, a
// bundle) is not split, and the record-time evaluation stands as the named bounded fallback.
struct MemoryWaitCounts {
    std::uint64_t waits_unmet = 0;       // slot-74 waits the model found unmet at record time
    std::uint64_t split = 0;             // lists split at a wait
    std::uint64_t split_refused = 0;     // waits whose list could not be split (named on stderr, bounded lines)
    std::uint64_t record_waits_met = 0;  // record-time waits a later recorded bottom-of-pipe write met
    std::uint64_t record_waits_cross_list = 0;  // of those, the writer was a different list (the measured case)
    std::uint64_t record_waits_same_list = 0;   // of those, the writer was later in the same list
    std::uint64_t segments = 0;          // submissions the split batches became
    std::uint64_t submits_split = 0;     // ExecuteCommandLists calls that carried a split list
    std::uint64_t submit_fallback = 0;   // a batch that could not be planned/held and was submitted unsplit
    std::uint64_t holds_created = 0;     // fences a held segment waits on
    std::uint64_t holds_satisfied = 0;   // released because the cell held (the watcher's poll or a recorded write)
    std::uint64_t holds_timeout = 0;     // released at the bound (the peers' bounded fallback)
    std::uint64_t holds_cancelled = 0;   // released because the queue was destroyed
    std::uint64_t holds_written = 0;     // holds a later bottom-of-pipe write met early
    std::uint64_t holds_cross_list = 0;  // of those, the writer was a different list (the measured cross-list case)
    std::uint64_t holds_at_bound = 0;    // the hold records' timed-out total (same as holds_timeout)
    std::uint64_t gpu_writes = 0;        // slot 71/72 values recorded as GPU-ordered immediate writes
    std::uint64_t gpu_refused = 0;       // GPU writes refused by shape (the CPU record-time write was used instead)
    std::uint64_t gpu_addresses = 0;     // distinct pages the GPU write form was used on
    std::uint64_t pending_holds = 0;     // holds the watcher has not released yet
};
MemoryWaitCounts memory_wait_counts();
// 1: on, 0: off, -1: the environment switch (the default). Tests use these to exercise both arms in one process.
void set_memory_wait_hold_enabled(int mode);
void set_memory_wait_gpu_writes_enabled(int mode);
void set_memory_wait_bound_ms(std::uint64_t milliseconds);

// ---- Allocator slot 9: invoking the registered callback ----------------------------------------------------------
//
// docs/allocator_slot9.md. The registered callback game+0x555DF10 reads: argument 4, the size (2^arg4 when arg4 < 16,
// else explicit bytes), argument 3, the recording pool's memory class (passed on as edx), and argument 6, the
// recording context (the registration's context); it allocates a block from the pool at context+0x12780, frees the
// previous block at context+0x12798 and stores the new one there. The title itself allocates from that pool with
// class 0x18 (game+0x18845E), and a static scan shows context+0x12798 is read only by the callback and the callback
// pointer is referenced only at the registration site, so the console runtime is the only invoker. With
// PALMETTO_XS_ALLOCATOR_CALLBACK=1 (off by default) the proxy invokes the pair once per allocator proxy, on its first
// registration, with (0, 0, 0, class 0x18, size 0x10000, 0, context) and logs the returned block and the cell; a null
// return, an unreadable context or a context that already carries a block is refused by name. The invocation is a
// parity step: no measured title code reads the block back.
struct AllocatorCallbackCounts {
    std::uint64_t registrations = 0, first_registrations = 0, invocations = 0, refusals = 0;
};
AllocatorCallbackCounts allocator_callback_counts();
// 1: on, 0: off, -1: the PALMETTO_XS_ALLOCATOR_CALLBACK environment switch (the default). Tests use this.
void set_allocator_callback_invocation_enabled(int mode);
bool allocator_callback_invocation_enabled();

// Command list proxies created by device slot 73 (CreateCommandListX):
bool is_command_list_proxy(const void* list);
ID3D12GraphicsCommandList* command_list_proxy_real_list(const void* list);
unsigned command_list_proxy_table_size();
std::uint64_t list_slot_hits(unsigned slot);
bool list_slot_first_call_seen(unsigned slot);

// Heap slots 0-2, 7 (GetDevice -> device proxy) and 9/10 (handle conversion).
bool heap_slot_overridden(unsigned slot);

// Console list slot mapping: returns PC vtable slot or ~0u if console-only / unforwarded.
inline constexpr unsigned kListNotForwarded = ~0u;
inline constexpr unsigned console_to_pc_list_slot(unsigned slot) {
    if (slot <= 26) return slot;               // PC order, measured
    if (slot <= 30) return kListNotForwarded;  // console-only extras
    if (slot <= 59) return slot - 4;           // PC chain, shifted +4
    if (slot == 60) return 58;                 // EndEvent (no console SetMarker/BeginEvent)
    if (slot == 61) return 59;                 // ExecuteIndirect (Package)
    if (slot == 85) return 16;                 // CopyTextureRegionX -> CopyTextureRegion
    if (slot == 146) return 80;                // D3D12_BARRIER_GROUP (Package; PC GCL7)
    return kListNotForwarded;
}


// Number of live references on a device proxy. Test helper for the heap's GetDevice/lifetime checks.
unsigned long device_proxy_ref_count(const void* device);

// Per-slot call counters and first-call flags for the device and heap proxies. Slot 0 also counts the
// QueryInterface call made while a proxy is created, so tests reset the counters after creation.
// reset_device_proxy_stats also resets the queue and fence counters.
// The hit counters (device, heap, queue, fence and allocator) are for tests, and they are off at the start.
// A test must read a counter (any *_slot_hits function) or call reset_device_proxy_stats BEFORE the calls that it
// counts. Calls made before that are not counted (the counter stays 0); they set only the first-call flags.
std::uint64_t device_slot_hits(unsigned slot);
bool device_slot_first_call_seen(unsigned slot);
std::uint64_t heap_slot_hits(unsigned slot);
bool heap_slot_first_call_seen(unsigned slot);
void reset_device_proxy_stats();

}  // namespace rt::d3d12_xs

// Called once by the Vulkan layer at startup with its rt::d3d12_xs::FenceValueWatcherFn (a void* so this header does
// not have to name the function-pointer type twice; the layer checks the type). See console fence slot 12 above.
extern "C" void palmetto_register_fence_value_watcher(void* fn);
