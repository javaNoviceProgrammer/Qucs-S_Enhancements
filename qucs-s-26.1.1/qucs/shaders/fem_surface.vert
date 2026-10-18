// A surface's values, (v - min) / (max - min) at each vertex.
#version 440
layout(location = 0) in vec2 position;
layout(location = 1) in float value;
layout(location = 0) out float vValue;
layout(std140, binding = 0) uniform Buf {
    mat4 corr;
    vec2 center;
    vec2 scale;
};
void main()
{
    gl_Position = corr * vec4((position - center) * scale, 0.0, 1.0);
    vValue = value;
}
