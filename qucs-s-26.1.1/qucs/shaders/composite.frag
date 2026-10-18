// Premultiplied, as QImage's RGBA8888_Premultiplied.
#version 440
layout(location = 0) in vec2 vTexcoord;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform Buf {
    mat4 corr;
    float opacity;
};
layout(binding = 1) uniform sampler2D image;
void main()
{
    fragColor = texture(image, vTexcoord) * opacity;
}
