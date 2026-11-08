// A frame's edge as KLayout draws it, 'lineWidth' device pixels wide: a
// quad an instance, its ends out by half the width (the corners joined
// square), on the pixels QPainter inks (half a pixel right and down, a
// whole one when the width is even); 'along' the pixels from the screen's
// left (an edge more across than up) or its bottom, for the dashes.
#version 440
layout(location = 0) in vec2 corner;   // ±0.5: x which end (-: the first), y which side
layout(location = 1) in vec4 seg;      // x0 y0 x1 y1, µm from the origin
layout(location = 0) out float along;
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
    float len = length(d);
    vec2 u = len > 0.0 ? d / len : vec2(1.0, 0.0);
    vec2 n = vec2(-u.y, u.x);
    float shift = mod(lineWidth, 2.0) > 0.5 ? 0.5 : 1.0;
    vec2 p = (corner.x < 0.0 ? a : b) + u * (corner.x * lineWidth) + n * (corner.y * lineWidth) + vec2(shift, -shift);
    along = abs(d.x) >= abs(d.y) ? p.x + 0.5 * viewport.x : p.y + 0.5 * viewport.y;
    gl_Position = corr * vec4(p * 2.0 / viewport, 0.0, 1.0);
}
