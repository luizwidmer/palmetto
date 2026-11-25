#pragma once

// Format and resource-flag helpers shared by the Vulkan-backed device (port/shims/d3d12_vulkan_device.cpp) and its
// GPU-free tests. They were file-local in the device; the console translation below needs the same format-family
// knowledge the device's own validation uses, so both live here.
//
// The console's placed creation (Xbox device slot 133, CreatePlacedResourceX) forwards a description whose resource
// flags the proxy has already masked down to the PC flag bits. The console lets one of those pairs through that the
// host PC runtime refuses: a depth-stencil texture that also allows unordered access. The console uses it for depth
// buffers its shaders also read or write as typed UAVs. The host runtime answers that pair with E_INVALIDARG
// ("resource flag combination" in the device's own words), so a title that relies on the console's pair gets a null
// resource. `console_depth_uav_translated` is the measured translation for that pair on the console path: the
// resource becomes the depth-stencil texture it is used as, and the unordered-access half stays refused by name where
// a view asks for it.
//
// Measured on the title's loading stage: three slot-133 descriptions, format R16_TYPELESS (53) with
// ALLOW_DEPTH_STENCIL | ALLOW_UNORDERED_ACCESS (0x6), 2D, one mip, one sample, 6/1/3 array layers. No game data is in
// this header: format numbers, flag bits and reasons only.

#include <d3d12.h>

namespace palmetto {

// The typeless format a DXGI format can be cast within, or the format itself when it has no such family.
inline DXGI_FORMAT format_family(DXGI_FORMAT format) {
    const int f = int(format);
    const auto in = [f](int first, int last) { return f >= first && f <= last; };
    if (in(1, 4)) return DXGI_FORMAT_R32G32B32A32_TYPELESS;
    if (in(5, 8)) return DXGI_FORMAT_R32G32B32_TYPELESS;
    if (in(9, 14)) return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    if (in(15, 18)) return DXGI_FORMAT_R32G32_TYPELESS;
    if (in(19, 22)) return DXGI_FORMAT_R32G8X24_TYPELESS;
    if (in(23, 25)) return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    if (in(27, 32)) return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    if (in(33, 38)) return DXGI_FORMAT_R16G16_TYPELESS;
    if (in(39, 43)) return DXGI_FORMAT_R32_TYPELESS;
    if (in(44, 47)) return DXGI_FORMAT_R24G8_TYPELESS;
    if (in(48, 52)) return DXGI_FORMAT_R8G8_TYPELESS;
    if (in(53, 59)) return DXGI_FORMAT_R16_TYPELESS;
    if (in(60, 64)) return DXGI_FORMAT_R8_TYPELESS;
    if (in(70, 72)) return DXGI_FORMAT_BC1_TYPELESS;
    if (in(73, 75)) return DXGI_FORMAT_BC2_TYPELESS;
    if (in(76, 78)) return DXGI_FORMAT_BC3_TYPELESS;
    if (in(79, 81)) return DXGI_FORMAT_BC4_TYPELESS;
    if (in(82, 84)) return DXGI_FORMAT_BC5_TYPELESS;
    if (f == 87 || in(90, 91)) return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    if (f == 88 || in(92, 93)) return DXGI_FORMAT_B8G8R8X8_TYPELESS;
    if (in(94, 96)) return DXGI_FORMAT_BC6H_TYPELESS;
    if (in(97, 99)) return DXGI_FORMAT_BC7_TYPELESS;
    return format;
}

// ALLOW_DEPTH_STENCIL: the host accepts every format of a family with a depth member (R32_FLOAT and R16_FLOAT
// included) and nothing else.
inline bool depth_stencil_family(DXGI_FORMAT format) {
    const DXGI_FORMAT family = format_family(format);
    return family == DXGI_FORMAT_R32_TYPELESS || family == DXGI_FORMAT_R16_TYPELESS ||
           family == DXGI_FORMAT_R24G8_TYPELESS || family == DXGI_FORMAT_R32G8X24_TYPELESS;
}

// The console-valid depth-stencil + unordered-access pair, translated for the console's placed-creation path.
// `desc` is the description as the console proxy hands it on (PC flag bits only, layout replaced); when this returns
// true its unordered-access flag has been cleared, so the device creates the depth-stencil texture the title renders
// into, and a later unordered-access view stays refused by name (the layer cannot make a Vulkan storage view of a
// depth format). A description that is not a depth-stencil texture, or does not ask for unordered access, is left
// exactly as it was: the host's own rules still refuse genuinely invalid combinations (a colour texture with
// ALLOW_DEPTH_STENCIL, a buffer with either flag, and so on).
inline bool console_depth_uav_translated(D3D12_RESOURCE_DESC& desc) {
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) return false;
    if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) == 0) return false;
    if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0) return false;
    if (!depth_stencil_family(desc.Format)) return false;
    desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
        UINT(desc.Flags) & ~UINT(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
    return true;
}

}  // namespace palmetto
