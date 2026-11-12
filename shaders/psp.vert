#version 450
/* PSP GE vertex transform: pixel coordinates to clip space.
 * 480x272 screen, z is a 16-bit depth value. */

layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec2 a_uv;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;

void main() {
    gl_Position = vec4(a_pos.x * (1.0 / 240.0) - 1.0,
                       a_pos.y * (1.0 / 136.0) - 1.0,
                       clamp(a_pos.z, 0.0, 65535.0) / 65535.0,
                       1.0);
    v_color = a_color;
    v_uv = a_uv;
}
