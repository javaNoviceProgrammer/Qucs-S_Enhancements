#version 440
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform Buf {
    mat4 corr;
    vec4 color;
    vec2 center;
    vec2 scale;
    vec2 nudge;
    vec2 viewport;
    float pixels;
    float cell;
    float patterned;
    float yUp;
};
void main()
{
    fragColor = color;
}
