// Filled shapes in clip space (y up), each vertex its premultiplied color.
#version 440
layout(location = 0) in vec2 position;
layout(location = 1) in vec4 color;
layout(location = 0) out vec4 vColor;
layout(std140, binding = 0) uniform Buf {
    mat4 corr;
};
void main()
{
    vColor = color;
    gl_Position = corr * vec4(position, 0.0, 1.0);
}
