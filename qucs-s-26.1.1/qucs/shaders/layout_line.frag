// A frame's dashes, as KLayout counts them: its pixel x from the screen's
// left (an edge more across than up) or b from its bottom, a dash's pixel
// W (the line's width) long - drawn where the bit of floor((x + lo) / W),
// or floor((b + hi) / W), is set (lo = (W - 1) / 2, hi = W / 2).
#version 440
layout(location = 0) in float across;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform Buf {
    mat4 corr;
    vec4 color;        // straight alpha
    vec2 center;
    vec2 scale;
    vec2 nudge;
    vec2 viewport;     // device pixels
    float pixels;      // device pixels a µm
    float cell;        // a pattern's pixel in device pixels
    float patterned;
    float yUp;         // the framebuffer's rows upwards (OpenGL's)
    vec2 phase;        // (anchored) the pattern moved on, its pixels (KLayout's scrolling)
    vec2 dotSize;      // a dot's quad, device pixels
    float anchored;    // the pattern from the lower left, its last row on the bottom one (KLayout's); else from the top left (Qt's)
    float lineWidth;   // a thick line's, device pixels
    float dashLength;  // its dashes' period, the pattern's pixels; 0: solid
    float always;      // dots whatever their shape's size (a corner's mark)
    vec4 dash;         // the dashes' bits, 8 a component
};
void main()
{
    if (dashLength > 0.5) {
        float w = max(lineWidth, 1.0);
        float x = floor(gl_FragCoord.x);
        float fromTop = yUp > 0.5 ? viewport.y - 1.0 - floor(gl_FragCoord.y) : floor(gl_FragCoord.y);
        float b = viewport.y - 1.0 - fromTop;
        float t = across > 0.5 ? floor((x + floor((w - 1.0) / 2.0)) / w) : floor((b + floor(w / 2.0)) / w);
        int i = int(mod(t, dashLength));
        int k = i / 8;
        // (its bit by integers: a float's byte / exp2(n) floored lost a
        // bit where exp2 came out a hair over - Metal's 14 / 2 was 6)
        int byte = int(k == 0 ? dash.x : k == 1 ? dash.y : k == 2 ? dash.z : dash.w);
        if (((byte >> (i - k * 8)) & 1) == 0) discard;
    }
    fragColor = color;
}
