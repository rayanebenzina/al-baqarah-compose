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

bool insideAt(vec2 p, int offset, int count) {
    int winding = 0;
    for (int i = 0; i < count; ++i) {
        int idx = (offset + i) * 3;
        winding += windingForCurve(p, pts[idx + 0], pts[idx + 1], pts[idx + 2]);
    }
    return winding != 0;
}

void main() {
    vec4 e0 = entries[vLayer * 2 + 0];
    vec4 color = entries[vLayer * 2 + 1];
    int offset = int(e0.x);
    int count = int(e0.y);

    if (count <= 0) discard;

    // 4x rotated-grid supersampling
    vec2 duvdx = dFdx(vUV);
    vec2 duvdy = dFdy(vUV);
    vec2 s0 = vUV + duvdx * 0.125 + duvdy * 0.375;
    vec2 s1 = vUV + duvdx * 0.375 + duvdy * -0.125;
    vec2 s2 = vUV + duvdx * -0.125 + duvdy * -0.375;
    vec2 s3 = vUV + duvdx * -0.375 + duvdy * 0.125;

    float cov = 0.0;
    if (insideAt(s0, offset, count)) cov += 0.25;
    if (insideAt(s1, offset, count)) cov += 0.25;
    if (insideAt(s2, offset, count)) cov += 0.25;
    if (insideAt(s3, offset, count)) cov += 0.25;

    if (cov <= 0.001) discard;
    outColor = vec4(color.rgb, color.a * cov);
}
