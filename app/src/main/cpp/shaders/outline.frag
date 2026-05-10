#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(std430, set = 0, binding = 0) readonly buffer Curves {
    // Tightly packed quadratic Beziers: 3 vec2 per curve (p0, p1, p2).
    vec2 pts[];
};

layout(push_constant) uniform PC {
    vec2 viewport;
    float scrollY;
    int curveCount;
} pc;

const float kEps = 1e-6;

// For a single quadratic Bezier (a, b, c), count even-odd crossings to
// the right of point p (i.e. roots of y(t) = p.y with x(t) > p.x).
int crossingsForCurve(vec2 p, vec2 a, vec2 b, vec2 c) {
    float A = a.y - 2.0 * b.y + c.y;
    float B = 2.0 * (b.y - a.y);
    float C = a.y - p.y;

    int hits = 0;

    if (abs(A) < kEps) {
        if (abs(B) < kEps) return 0;
        float t = -C / B;
        if (t >= 0.0 && t <= 1.0) {
            float Ax = a.x - 2.0 * b.x + c.x;
            float Bx = 2.0 * (b.x - a.x);
            float Cx = a.x;
            float x = Ax * t * t + Bx * t + Cx;
            if (x > p.x) hits++;
        }
    } else {
        float disc = B * B - 4.0 * A * C;
        if (disc < 0.0) return 0;
        float sq = sqrt(disc);
        float invA = 1.0 / (2.0 * A);
        float t0 = (-B - sq) * invA;
        float t1 = (-B + sq) * invA;

        float Ax = a.x - 2.0 * b.x + c.x;
        float Bx = 2.0 * (b.x - a.x);
        float Cx = a.x;

        if (t0 >= 0.0 && t0 <= 1.0) {
            float x = Ax * t0 * t0 + Bx * t0 + Cx;
            if (x > p.x) hits++;
        }
        if (t1 >= 0.0 && t1 <= 1.0) {
            float x = Ax * t1 * t1 + Bx * t1 + Cx;
            if (x > p.x) hits++;
        }
    }
    return hits;
}

bool insideAt(vec2 p) {
    int crossings = 0;
    for (int i = 0; i < pc.curveCount; ++i) {
        vec2 a = pts[i * 3 + 0];
        vec2 b = pts[i * 3 + 1];
        vec2 c = pts[i * 3 + 2];
        crossings += crossingsForCurve(p, a, b, c);
    }
    return (crossings & 1) == 1;
}

void main() {
    // 4x rotated-grid supersampling — sample inside/outside at 4 sub-pixel
    // offsets and average. dFdx/dFdy give the per-pixel UV step, so we
    // shift in UV space by quarter-pixel amounts.
    vec2 duvdx = dFdx(vUV);
    vec2 duvdy = dFdy(vUV);

    // Rotated 2x2 grid: offsets ±1/8 and ±3/8 of a pixel
    vec2 s0 = vUV + duvdx * 0.125 + duvdy * 0.375;
    vec2 s1 = vUV + duvdx * 0.375 + duvdy * -0.125;
    vec2 s2 = vUV + duvdx * -0.125 + duvdy * -0.375;
    vec2 s3 = vUV + duvdx * -0.375 + duvdy * 0.125;

    float cov = 0.0;
    if (insideAt(s0)) cov += 0.25;
    if (insideAt(s1)) cov += 0.25;
    if (insideAt(s2)) cov += 0.25;
    if (insideAt(s3)) cov += 0.25;

    if (cov <= 0.001) discard;
    outColor = vec4(0.96, 0.92, 0.78, cov);
}
