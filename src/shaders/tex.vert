#version 440
// Textured quad for the retained GPU backend's raster layer. Same camera uniform
// as color.vert (vec4 cam = centre.xy + NDC-per-metre.zw); passes UV to the frag.
layout(location = 0) in vec2 position;
layout(location = 1) in vec2 uv;
layout(location = 0) out vec2 v_uv;
layout(std140, binding = 0) uniform Camera { vec4 cam; vec4 rot; } u;
void main() {
    v_uv = uv;
    vec2 d = position - u.cam.xy;
    vec2 dr = vec2(d.x * u.rot.x - d.y * u.rot.y,
                   d.x * u.rot.y + d.y * u.rot.x);
    vec2 ndc = dr * u.cam.zw;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
