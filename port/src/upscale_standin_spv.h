#pragma once
#include <cstdint>

namespace palmetto {

// The upscale stand-in's two stages (docs/upscale_standin.md), compiled from upscale_standin.vert/frag with
//   glslc --target-env=vulkan1.1 -O -mfmt=c upscale_standin.vert -o upscale_standin_vs.inc
//   glslc --target-env=vulkan1.1 -O -mfmt=c upscale_standin.frag -o upscale_standin_ps.inc
// The layer's own pipeline reads them; the stand-in never uses a title shader.
inline constexpr std::uint32_t upscale_standin_vs_spirv[] =
#include "upscale_standin_vs.inc"
    ;

inline constexpr std::uint32_t upscale_standin_ps_spirv[] =
#include "upscale_standin_ps.inc"
    ;

}  // namespace palmetto
