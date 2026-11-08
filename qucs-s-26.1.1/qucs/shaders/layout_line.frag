// A frame's dashes: the t'th of the pattern's pixels along it drawn where
// its bit is set.
#version 440
layout(location = 0) in float along;
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
        int t = int(mod(floor(along / cell), dashLength));
        int k = t / 8;
        float byte = k == 0 ? dash.x : k == 1 ? dash.y : k == 2 ? dash.z : dash.w;
        if (mod(floor(byte / exp2(float(t - k * 8))), 2.0) < 0.5) discard;
    }
    fragColor = color;
}
