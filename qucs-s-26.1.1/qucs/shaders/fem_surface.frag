// In color: the table's entry of the value interpolated, as the view's
// raster has it.
#version 440
layout(location = 0) in float vValue;
layout(location = 0) out vec4 fragColor;
layout(binding = 1) uniform sampler2D colormap;
void main()
{
    int i = clamp(int(vValue * 255.0 + 0.5), 0, 255);
    fragColor = texelFetch(colormap, ivec2(i, 0), 0);
}
