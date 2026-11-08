// A fill's pattern in device pixels: Qt's 8 by 8 bitmap from the top left,
// as QPainter draws a brush's - or KLayout's, any size up to 32 by 32,
// from the lower left (its last row on the bottom one), a pixel of it
// 'cell' device pixels, moved on by 'phase'.
#version 440
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
layout(binding = 1) uniform sampler2D pattern;
void main()
{
    if (patterned > 0.5) {
        ivec2 size = textureSize(pattern, 0);
        ivec2 at;
        if (anchored > 0.5) {
            float fromBottom = yUp > 0.5 ? gl_FragCoord.y : viewport.y - gl_FragCoord.y;
            vec2 p = floor(vec2(gl_FragCoord.x, fromBottom) / cell) - phase;
            at = ivec2(int(mod(p.x, float(size.x))), size.y - 1 - int(mod(p.y, float(size.y))));
        } else {
            float y = yUp > 0.5 ? viewport.y - gl_FragCoord.y : gl_FragCoord.y;
            at = ivec2(mod(floor(vec2(gl_FragCoord.x, y) / cell), vec2(size)));
        }
        if (texelFetch(pattern, at, 0).r < 0.5) discard;
    }
    fragColor = color;
}
