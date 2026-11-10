#version 440
layout(location = 0) in vec4 vColor;
layout(location = 1) in float vAlong;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform Buf {
    mat4 corr;
    vec4 right;
    vec4 up;
    vec4 toward;
    vec4 center;
    vec4 clip;
    vec2 scale;
    vec2 viewport;
    float depthRange;
    float bias;
    float clipOn;
    float pixel;
};
void main()
{
    if (clipOn > 0.5 && vAlong > clip.w) discard;
    fragColor = vColor;
}
