// The layout's fills and outlines: µm from the origin to clip space, y
// upwards as the layout has it. (Lines half a device pixel to the right -
// nudge - as QPainter draws an aliased one: on the pixels right of its
// mathematical place; fills none.) KLayout's look: each point on the device
// pixel KLayout puts it on first - round(x), the row above round(y) from
// the top -, a fill's ('anchored') at that pixel's top left corner, a
// line's ('always') at its middle (a hair right and up); then 'nudge'.
#version 440
layout(location = 0) in vec2 position;
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
    vec2 clip = (position - center) * scale;
    if (anchored > 0.5 || always > 0.5) {
        vec2 at = vec2((clip.x + 1.0) * 0.5 * viewport.x, (1.0 - clip.y) * 0.5 * viewport.y);   // device pixels, y down
        at = vec2(floor(at.x + 0.5), ceil(at.y - 0.5) - 1.0);
        if (always > 0.5) at += vec2(0.5 + 1.0 / 256.0, 0.5 - 1.0 / 256.0);
        clip = vec2(at.x / viewport.x * 2.0 - 1.0, 1.0 - at.y / viewport.y * 2.0);
    }
    gl_Position = corr * vec4(clip + nudge, 0.0, 1.0);
}
