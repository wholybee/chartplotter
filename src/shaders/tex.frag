#version 440
// Samples the composited raster-chart texture. The image is premultiplied-alpha
// ARGB uploaded as RGBA8, so straight sampling composites correctly over the sea
// clear beneath it.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;
layout(binding = 1) uniform sampler2D tex;
void main() {
    fragColor = texture(tex, v_uv);
}
