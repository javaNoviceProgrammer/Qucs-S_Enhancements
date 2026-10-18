// A fill's pattern: Qt's 8 by 8 bitmap in device pixels from the top left,
// as QPainter draws a brush's.
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
layout(binding = 1) uniform sampler2D pattern;
void main()
{
    if (patterned > 0.5) {
        float y = yUp > 0.5 ? viewport.y - gl_FragCoord.y : gl_FragCoord.y;
        ivec2 at = ivec2(mod(floor(vec2(gl_FragCoord.x, y) / cell), 8.0));
        if (texelFetch(pattern, at, 0).r < 0.5) discard;
    }
    fragColor = color;
}
