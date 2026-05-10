#version 450

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in int inLayer;

layout(push_constant) uniform PC {
    vec2 viewport;
    float scrollY;
    float _pad;
} pc;

layout(location = 0) out vec2 vUV;
layout(location = 1) flat out int vLayer;

void main() {
    vec2 pos = vec2(inPos.x, inPos.y - pc.scrollY);
    vec2 ndc = (pos / pc.viewport) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    vUV = inUV;
    vLayer = inLayer;
}
