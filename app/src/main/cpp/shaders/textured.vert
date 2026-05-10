#version 450

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;

layout(push_constant) uniform PC {
    vec2 viewport;
} pc;

layout(location = 0) out vec2 vUV;

void main() {
    vec2 ndc = (inPos / pc.viewport) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    vUV = inUV;
}
