// Lines: each segment an instance, a quad round it in pixels; its pixels
// covered by their distance from it (antialiased, any width), with round
// or butt ends. Clipped to the viewport first: a segment thousands of
// screens long, deep in, keeps its pixels' floats exact. Pixels here from
// the bottom left, y up (the fragment shader's gl_FragCoord brought to them).
#version 440
layout(location = 0) in float corner;   // 0 to 3, a strip's
layout(location = 1) in vec4 segment;
layout(location = 2) in vec4 color;
layout(location = 3) in float width;
layout(location = 0) flat out vec2 a;
layout(location = 1) flat out vec2 b;
layout(location = 2) flat out float halfWidth;
layout(location = 3) flat out vec4 vColor;
layout(std140, binding = 0) uniform Buf {
    mat4 corr;
    vec4 uniformColor;
    vec2 center;
    vec2 scale;
    vec2 viewport;      // device pixels
    float ratio;        // device pixels a width's unit
    float roundCaps;
    float useUniform;   // the color and width the uniforms', not the instance's
    float uniformWidth;
    float yUp;
};
void main()
{
    vec2 p0 = ((segment.xy - center) * scale * 0.5 + 0.5) * viewport;
    vec2 p1 = ((segment.zw - center) * scale * 0.5 + 0.5) * viewport;
    float w = (useUniform > 0.5 ? uniformWidth : width) * ratio;
    vColor = useUniform > 0.5 ? uniformColor : color;
    if (w < 1.0) {
        vColor.a *= w;
        w = 1.0;
    }
    halfWidth = 0.5 * w;
    float r = halfWidth + 1.0;
    // (Liang and Barsky's clip to the viewport and a margin.)
    vec2 d = p1 - p0;
    float t0 = 0.0, t1 = 1.0;
    vec2 lo = vec2(-r - 2.0), hi = viewport + vec2(r + 2.0);
    for (int k = 0; k < 2; ++k) {
        if (abs(d[k]) < 1e-9) {
            if (p0[k] < lo[k] || p0[k] > hi[k]) t1 = -1.0;
        } else {
            float ta = (lo[k] - p0[k]) / d[k], tb = (hi[k] - p0[k]) / d[k];
            t0 = max(t0, min(ta, tb));
            t1 = min(t1, max(ta, tb));
        }
    }
    if (t1 < t0) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        a = vec2(-1e6);
        b = vec2(-1e6);
        return;
    }
    vec2 q0 = p0 + d * t0, q1 = p0 + d * t1;
    vec2 e = q1 - q0;
    float len = length(e);
    vec2 t = len > 1e-6 ? e / len : vec2(1.0, 0.0);
    vec2 n = vec2(-t.y, t.x);
    int c = int(corner + 0.5);
    vec2 at = (c < 2 ? q0 - t * r : q1 + t * r) + n * ((c == 0 || c == 2) ? -r : r);
    // (The ends kept where the segment's are: a clipped end a butt off screen.)
    a = t0 > 0.0 ? q0 - t * r : q0;
    b = t1 < 1.0 ? q1 + t * r : q1;
    gl_Position = corr * vec4(at / viewport * 2.0 - 1.0, 0.0, 1.0);
}
