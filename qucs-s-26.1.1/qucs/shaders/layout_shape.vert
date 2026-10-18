// The layout's fills and outlines: µm from the origin to clip space, y
// upwards as the layout has it. (Lines half a device pixel to the right -
// nudge - as QPainter draws an aliased one: on the pixels right of its
// mathematical place; fills none.)
#version 440
layout(location = 0) in vec2 position;
layout(std140, binding = 0) uniform Buf {
    mat4 corr;
    vec4 color;      // straight alpha
    vec2 center;
    vec2 scale;
    vec2 nudge;
    vec2 viewport;   // device pixels
    float pixels;    // device pixels a µm
    float cell;      // a pattern's pixel in device pixels
    float patterned;
    float yUp;       // the framebuffer's rows upwards (OpenGL's)
};
void main()
{
    gl_Position = corr * vec4((position - center) * scale + nudge, 0.0, 1.0);
}
