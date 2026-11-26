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

#include <string>

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

// ---- The console Series-S upscale output shape (docs/decomp_renderer.md section 12) --------------------------------
//
// Measured: with the console type answered as Series S (PALMETTO_CONSOLE_TYPE=6) the title's kind-7 FidelityFX branch
// creates its upscale output through the console placed-creation path (device slot 133): a 1920x1080 TEXTURE2D in
// DXGI_FORMAT_R9G9B9E5_SHAREDEXP with ALLOW_RENDER_TARGET | ALLOW_UNORDERED_ACCESS, at a console VA the title placed
// it at. The console title asks for that pair (the same format's UAV creation is measured in the FidelityFX route
// test, docs/d3d12_vulkan.md Appendix A and docs/fsr_route.md section 4.2); this slice measures the host's
// refusal, not the console's answer to the render-target half. The host devices this layer runs on do not take the
// format: on the measured RTX 4090 VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 has neither COLOR_ATTACHMENT nor STORAGE_IMAGE
// support, so the same description fails the allocation query and the placed creation answers 0x80070057, before any
// FidelityFX program is requested.
//
// `console_r9g9b9e5_render_target` names that one measured console shape. The device backs it with the same mutable
// R32_UINT image its UAV-only R9G9B9E5 textures already use (the E5B9G9R9 sampled view and the R32_UINT storage view
// are unchanged), and refuses the render-target view by name: the host cannot attach E5B9G9R9, and an R32_UINT
// attachment would not be the format the title's pipeline names. The rule is the format/flag pair the console's format
// and view support allows; the measured instance is the 1920x1080, 1-mip, 1-layer, single-sample description.
inline bool console_r9g9b9e5_render_target(const D3D12_RESOURCE_DESC& desc) {
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return false;
    if (desc.Format != DXGI_FORMAT_R9G9B9E5_SHAREDEXP) return false;
    if (desc.SampleDesc.Count != 1 || desc.MipLevels != 1 || desc.DepthOrArraySize != 1) return false;
    constexpr UINT both =
        UINT(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) | UINT(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const UINT flags = UINT(desc.Flags);
    if ((flags & both) != both) return false;
    if (flags & (UINT(D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) | UINT(D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)))
        return false;
    return true;
}

// The console-type switch (`PALMETTO_CONSOLE_TYPE`) as the one value this translation is gated on. The loader owns the
// switch and its answers (port/src/pe_loader.cpp, `parse_console_type_switch`): it accepts `6`, `seriess`, `series-s`
// and `series s` for Series S, in any case and with surrounding spaces, and every other spelling leaves the default
// path. The device needs the same gate but is also built into GPU-free test binaries that do not link the loader, so
// it parses the Series-S spellings here; the two must stay in step. Pure, so the gate is tested without a GPU.
inline bool console_type_is_series_s(const char* text) {
    if (!text) return false;
    std::string value(text);
    const auto first = value.find_first_not_of(" \t");
    const auto last = value.find_last_not_of(" \t");
    value = first == std::string::npos ? std::string() : value.substr(first, last - first + 1);
    for (char& c : value)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return value == "6" || value == "seriess" || value == "series-s" || value == "series s";
}

}  // namespace palmetto
