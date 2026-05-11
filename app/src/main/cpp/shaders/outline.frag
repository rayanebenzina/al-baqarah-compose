#version 450

layout(location = 0) in vec2 vUV;
layout(location = 1) flat in int vLayer;
layout(location = 0) out vec4 outColor;

layout(std430, set = 0, binding = 0) readonly buffer Curves {
    // 3 vec2 per curve (p0, p1, p2). Length = totalCurveCount * 3.
    vec2 pts[];
};

layout(std430, set = 0, binding = 1) readonly buffer Layers {
    // 2 vec4 per layer:
    //   layer[2*i + 0] = (curveOffset, curveCount, _, _)  (ints reinterpreted as floats)
    //   layer[2*i + 1] = rgba
    vec4 entries[];
};

layout(std430, set = 0, binding = 2) readonly buffer Bands {
    // ivec2 per band; layerCount * NUM_BANDS entries laid out
    // contiguously. Entry (layer, band) is at index
    // (layer * NUM_BANDS + band), giving (offset, count) into
    // `indices[]` for the curves that touch that band.
    ivec2 bands[];
};

layout(std430, set = 0, binding = 3) readonly buffer CurveIndices {
    // Flat int list. Each entry is a curve index local to the
    // owning layer (0..layer.curveCount-1); add layer.curveOffset
    // to get the global index into `pts[]`.
    int indices[];
};

const int NUM_BANDS = 32;
const float kEps = 1e-6;

// Non-zero-winding contribution from a quadratic Bezier to a horizontal
// ray going to the +X direction from `p`. Returns +1 for upward crossings
// (dy/dt > 0), -1 for downward, 0 for none.
int windingForCurve(vec2 p, vec2 a, vec2 b, vec2 c) {
    float A = a.y - 2.0 * b.y + c.y;
    float B = 2.0 * (b.y - a.y);
    float C = a.y - p.y;

    int w = 0;
    float Ax = a.x - 2.0 * b.x + c.x;
    float Bx = 2.0 * (b.x - a.x);
    float Cx = a.x;

    if (abs(A) < kEps) {
        if (abs(B) < kEps) return 0;
        float t = -C / B;
        if (t >= 0.0 && t <= 1.0) {
            float x = Ax * t * t + Bx * t + Cx;
            if (x > p.x) {
                // dy/dt = B (constant for degenerate quadratic)
                w += (B > 0.0) ? 1 : -1;
            }
        }
    } else {
        float disc = B * B - 4.0 * A * C;
        if (disc < 0.0) return 0;
        float sq = sqrt(disc);
        float invA = 1.0 / (2.0 * A);
        float t0 = (-B - sq) * invA;
        float t1 = (-B + sq) * invA;

        if (t0 >= 0.0 && t0 <= 1.0) {
            float x = Ax * t0 * t0 + Bx * t0 + Cx;
            if (x > p.x) {
                float dy = 2.0 * A * t0 + B;
                w += (dy > 0.0) ? 1 : -1;
            }
        }
        if (t1 >= 0.0 && t1 <= 1.0) {
            float x = Ax * t1 * t1 + Bx * t1 + Cx;
            if (x > p.x) {
                float dy = 2.0 * A * t1 + B;
                w += (dy > 0.0) ? 1 : -1;
            }
        }
    }
    return w;
}

bool insideAt(vec2 p, int curveOffset, int bandBase) {
    int bandIdx = clamp(int(p.y * float(NUM_BANDS)), 0, NUM_BANDS - 1);
    ivec2 br = bands[bandBase + bandIdx];
    int off = br.x;
    int cnt = br.y;

    int winding = 0;
    for (int i = 0; i < cnt; ++i) {
        int local = indices[off + i];
        int gi = (curveOffset + local) * 3;
        winding += windingForCurve(p, pts[gi + 0], pts[gi + 1], pts[gi + 2]);
    }
    return winding != 0;
}

void main() {
    vec4 e0 = entries[vLayer * 2 + 0];
    vec4 color = entries[vLayer * 2 + 1];
    int curveOffset = int(e0.x);
    int curveCount = int(e0.y);
    int bandBase = vLayer * NUM_BANDS;

    if (curveCount <= 0) discard;

    // 2x rotated-grid supersampling
    vec2 duvdx = dFdx(vUV);
    vec2 duvdy = dFdy(vUV);
    vec2 s0 = vUV + duvdx *  0.25 + duvdy *  0.25;
    vec2 s1 = vUV + duvdx * -0.25 + duvdy * -0.25;

    float cov = 0.0;
    if (insideAt(s0, curveOffset, bandBase)) cov += 0.5;
    if (insideAt(s1, curveOffset, bandBase)) cov += 0.5;

    if (cov <= 0.001) discard;
    outColor = vec4(color.rgb, color.a * cov);
}
