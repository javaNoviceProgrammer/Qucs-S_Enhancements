// An image on the canvas: a layer of QPainter's drawing, a PDF's tile, a
// Markdown preview's - its quad in clip space (y up), corrected for the
// backend (QRhi::clipSpaceCorrMatrix).
#version 440
layout(location = 0) in vec2 position;
layout(location = 1) in vec2 texcoord;
layout(location = 0) out vec2 vTexcoord;
layout(std140, binding = 0) uniform Buf {
    mat4 corr;
    float opacity;
};
void main()
{
    vTexcoord = texcoord;
    gl_Position = corr * vec4(position, 0.0, 1.0);
}
