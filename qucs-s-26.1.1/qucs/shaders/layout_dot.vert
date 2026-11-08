// A shape's dot: where the shape is under 1.5 pixels across, a quad
// 'dotSize' device pixels at its middle (else none) - each dot an instance;
// 'always', whatever its size (a corner's mark: a bar of a cross).
#version 440
layout(location = 0) in vec2 corner;   // ±0.5
layout(location = 1) in vec3 dot;      // x y (µm from the origin), its larger side (µm)
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
    if (always > 0.5 || dot.z * pixels < 1.5) {
        vec2 clip = (dot.xy - center) * scale + nudge + corner * 2.0 * dotSize / viewport;
        gl_Position = corr * vec4(clip, 0.0, 1.0);
    } else {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    }
}
