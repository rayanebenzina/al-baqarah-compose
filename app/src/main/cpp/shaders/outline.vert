#version 450

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;

layout(push_constant) uniform PC {
    vec2 viewport;
    float scrollY;
    int curveCount;
} pc;

layout(location = 0) out vec2 vUV;

void main() {
    vec2 pos = vec2(inPos.x, inPos.y - pc.scrollY);
    vec2 ndc = (pos / pc.viewport) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    vUV = inUV;
}
