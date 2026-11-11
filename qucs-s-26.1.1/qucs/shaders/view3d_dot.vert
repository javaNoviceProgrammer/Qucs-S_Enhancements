// Multiphysics 3D's dots: each an instance, a square round it of its radius
// in device pixels, a disc kept of it; drawn toward the eye by its radius
// and the lines' bias, its depth tested; beyond the Cut, none of it.
#version 440
layout(location = 0) in float corner;   // 0 to 3, a strip's
layout(location = 1) in vec3 position;
layout(location = 2) in vec4 color;
layout(location = 3) in float radius;   // device pixels
layout(location = 0) flat out vec4 vColor;
layout(location = 1) out vec2 vOffset;  // device pixels from the centre
layout(location = 2) flat out float vRadius;
layout(location = 3) flat out float vAlong;
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
    vec3 d = position - center.xyz;
    float r = max(1.0, radius);
    vec2 s = (vec2(dot(d, right.xyz) * scale.x, dot(d, up.xyz) * scale.y) * 0.5 + 0.5) * viewport;
    int c = int(corner + 0.5);
    vec2 offset = vec2((c == 0 || c == 2) ? -(r + 1.0) : r + 1.0, c < 2 ? -(r + 1.0) : r + 1.0);
    float z = -(dot(d, toward.xyz) + bias + 2.0 * r * pixel) / depthRange;
    gl_Position = corr * vec4((s + offset) / viewport * 2.0 - 1.0, z, 1.0);
    vColor = color;
    vOffset = offset;
    vRadius = r;
    vAlong = dot(position, clip.xyz);
}
