#version 440

layout(location = 0) in vec2 position;   // scene metres, relative to scene origin
layout(location = 1) in vec3 color;

layout(location = 0) out vec3 v_color;

// Camera: cam.xy = view centre (scene metres, relative to origin);
//         cam.zw = NDC units per scene metre on x and y (zoom + aspect).
layout(std140, binding = 0) uniform Camera {
    vec4 cam;
} u;

void main()
{
    v_color = color;
    vec2 ndc = (position - u.cam.xy) * u.cam.zw;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
