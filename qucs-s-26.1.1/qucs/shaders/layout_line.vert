// A frame's edge as KLayout draws it, 'lineWidth' (W) device pixels wide:
// a quad an instance. Along x or y, on KLayout's own pixels: its ends the
// pixels KLayout puts them on (round(x), the row above round(y) from the
// top), out by lo = (W - 1) / 2 (a solid one by hi = W / 2 after, its
// corners square), across it from lo before to hi after (an even width's
// extra pixel right or below) - the quad's corners on those pixels'
// edges. Slanted: its ends out by half the width, on the pixels QPainter
// inks (half a pixel right and down, a whole one when the width is even),
// moved by 'nudge' as the layer's fills are. 'across': more across than
// up, for the dashes.
#version 440
layout(location = 0) in vec2 corner;   // ±0.5: x which end (-: the first), y which side
layout(location = 1) in vec4 seg;      // x0 y0 x1 y1, µm from the origin
layout(location = 0) out float across;
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
    vec2 a = (seg.xy - center) * scale * 0.5 * viewport;   // device pixels from the middle, y up
    vec2 b = (seg.zw - center) * scale * 0.5 * viewport;
    vec2 d = b - a;
    across = abs(d.x) >= abs(d.y) ? 1.0 : 0.0;
    float w = max(lineWidth, 1.0);
    float lo = floor((w - 1.0) / 2.0);
    float hi = floor(w / 2.0);
    float after = dashLength > 0.5 ? lo : hi;
    vec2 p;
    if (abs(d.x) < 1e-3 || abs(d.y) < 1e-3) {
        // (device pixels from the top left, y down)
        vec2 A = vec2(a.x + 0.5 * viewport.x, 0.5 * viewport.y - a.y);
        vec2 B = vec2(b.x + 0.5 * viewport.x, 0.5 * viewport.y - b.y);
        float c0, c1, r0, r1;   // the pixels' columns and rows, both ends in
        if (abs(d.y) < 1e-3) {
            float row = ceil(A.y - 0.5) - 1.0;
            c0 = floor(min(A.x, B.x) + 0.5) - lo;
            c1 = floor(max(A.x, B.x) + 0.5) + after;
            r0 = row - lo;
            r1 = row + hi;
        } else {
            float column = floor(A.x + 0.5);
            r0 = ceil(min(A.y, B.y) - 0.5) - 1.0 - lo;
            r1 = ceil(max(A.y, B.y) - 0.5) - 1.0 + after;
            c0 = column - lo;
            c1 = column + hi;
        }
        p = vec2((corner.x < 0.0 ? c0 : c1 + 1.0) - 0.5 * viewport.x, 0.5 * viewport.y - (corner.y < 0.0 ? r0 : r1 + 1.0));
    } else {
        float len = length(d);
        vec2 u = len > 0.0 ? d / len : vec2(1.0, 0.0);
        vec2 n = vec2(-u.y, u.x);
        float shift = mod(lineWidth, 2.0) > 0.5 ? 0.5 : 1.0;
        p = (corner.x < 0.0 ? a : b) + u * (corner.x * lineWidth) + n * (corner.y * lineWidth) + vec2(shift, -shift) + nudge * 0.5 * viewport;
    }
    gl_Position = corr * vec4(p * 2.0 / viewport, 0.0, 1.0);
}
