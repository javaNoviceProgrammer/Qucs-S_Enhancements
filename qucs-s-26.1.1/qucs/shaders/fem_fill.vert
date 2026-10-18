// The multiphysics view's triangles - its domains, its mesh's quality -,
// each vertex its color (straight alpha).
#version 440
layout(location = 0) in vec2 position;
layout(location = 1) in vec4 color;
layout(location = 0) out vec4 vColor;
layout(std140, binding = 0) uniform Buf {
    mat4 corr;
    vec2 center;
    vec2 scale;
};
void main()
{
    gl_Position = corr * vec4((position - center) * scale, 0.0, 1.0);
    vColor = color;
}
