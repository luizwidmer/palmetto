#version 450
// The upscale stand-in's vertex stage (docs/upscale_standin.md): one full-screen triangle built from gl_VertexIndex, so
// the stand-in needs no vertex buffer, input layout or title pipeline state. Kept with upscale_standin.frag; both are
// compiled into upscale_standin_spv.h with
//   glslc --target-env=vulkan1.1 -O -mfmt=c upscale_standin.vert -o upscale_standin_vs.inc
void main() {
    const vec2 corner[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(corner[gl_VertexIndex], 0.0, 1.0);
}
