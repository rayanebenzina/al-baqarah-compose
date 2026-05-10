#version 450

layout(set = 0, binding = 0) uniform sampler2D sdfTex;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    float d = texture(sdfTex, vUV).r;
    float aa = fwidth(d) * 0.75;
    float a = smoothstep(0.5 - aa, 0.5 + aa, d);
    if (a <= 0.001) discard;
    outColor = vec4(0.96, 0.92, 0.78, a);
}
