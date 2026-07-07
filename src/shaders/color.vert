#version 440

layout(location = 0) in vec2 position;   // scene metres, relative to scene origin
layout(location = 1) in vec3 color;

layout(location = 0) out vec3 v_color;

// Camera: cam.xy = view centre (scene metres, relative to origin);
//         cam.zw = NDC units per scene metre on x and y (zoom + aspect);
//         rot.xy = (cos, sin) of the course-up rotation (0,1 vector = north-up).
layout(std140, binding = 0) uniform Camera {
    vec4 cam;
    vec4 rot;
} u;

void main()
{
    v_color = color;
    vec2 d = position - u.cam.xy;
    // Rotate in isotropic metre space, then apply the per-axis NDC scale (aspect).
    vec2 dr = vec2(d.x * u.rot.x - d.y * u.rot.y,
                   d.x * u.rot.y + d.y * u.rot.x);
    vec2 ndc = dr * u.cam.zw;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
