#version 450
#extension GL_EXT_samplerless_texture_functions : require
// The upscale stand-in's pixel stage (docs/upscale_standin.md): texel-fetch the colour image the title's upscale pass
// samples, from the image's whole texel rectangle stretched over the presented target's whole pixel rectangle. Nearest
// filtering reads only texels inside the source rectangle and copies the image's own values - the stand-in never
// invents colour; only the project's mapping control (mode 1) writes a coordinate instead, to prove the geometry when
// the title's colour image is black by design (docs/colour_path.md).
//
// The push block is the first 64 bytes of the 144-byte ShaderUserData block the replay pushes for every graphics draw
// (palmetto::upscale_standin::Push in upscale_standin.h; the two are kept in step and the unit test pins the formulas).
// Kept with upscale_standin.vert; both are compiled into upscale_standin_spv.h with
//   glslc --target-env=vulkan1.1 -O -mfmt=c upscale_standin.frag -o upscale_standin_ps.inc
layout(set = 0, binding = 0) uniform texture2D colour_image;
layout(location = 0) out vec4 out_colour;

layout(push_constant) uniform Push {
    vec4 src;  // the colour image's texel rectangle: left, top, right, bottom
    vec4 dst;  // the presented target's pixel rectangle: left, top, right, bottom
    uint mode; // 0: sample the colour image; 1: the project's mapping control
    uint target_width;
    uint target_height;
    uint pad;
} pc;

void main() {
    vec2 centre = gl_FragCoord.xy;  // the pixel centre, as map_dest_pixel() takes it
    vec2 u = (centre - pc.dst.xy) / (pc.dst.zw - pc.dst.xy);
    if (u.x < 0.0 || u.x >= 1.0 || u.y < 0.0 || u.y >= 1.0) {
        out_colour = vec4(0.0, 0.0, 0.0, 1.0);  // outside the destination rectangle: the layer writes nothing
        return;
    }
    if (pc.mode == 1u) {
        out_colour = vec4(u, 0.0, 1.0);  // the mapping control: the mapped source coordinate, not a colour
        return;
    }
    vec2 s = pc.src.xy + u * (pc.src.zw - pc.src.xy);
    vec2 lo = pc.src.xy;
    vec2 hi = max(pc.src.xy, pc.src.zw - vec2(1.0));
    ivec2 texel = ivec2(clamp(floor(s), lo, hi));
    out_colour = texelFetch(colour_image, texel, 0);
}
