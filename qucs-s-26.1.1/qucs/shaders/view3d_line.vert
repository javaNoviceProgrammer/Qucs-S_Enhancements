// Multiphysics 3D's lines: each segment an instance, a quad round it its
// width in device pixels (square ends, as the CPU's brush), drawn a little
// toward the eye - an edge on a face shows -, its depth tested; the Cut
// clips it where it crosses.
#version 440
layout(location = 0) in float corner;   // 0 to 3, a strip's
layout(location = 1) in vec3 a;
layout(location = 2) in vec3 b;
layout(location = 3) in vec4 color;
layout(location = 4) in float width;    // device pixels
layout(location = 0) flat out vec4 vColor;
layout(location = 1) out float vAlong;
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
vec3 project(vec3 p)
{
    vec3 d = p - center.xyz;
    return vec3(dot(d, right.xyz) * scale.x, dot(d, up.xyz) * scale.y, -(dot(d, toward.xyz) + bias) / depthRange);
}
void main()
{
    vec3 pa = project(a), pb = project(b);
    vec2 sa = (pa.xy * 0.5 + 0.5) * viewport, sb = (pb.xy * 0.5 + 0.5) * viewport;
    vec2 e = sb - sa;
    float len = length(e);
    vec2 t = len > 1e-6 ? e / len : vec2(1.0, 0.0);
    vec2 n = vec2(-t.y, t.x);
    float h = 0.5 * max(1.0, width);
    int c = int(corner + 0.5);
    bool atB = c >= 2;
    vec2 at = (atB ? sb + t * h : sa - t * h) + n * ((c == 0 || c == 2) ? -h : h);
    float z = atB ? pb.z : pa.z;
    gl_Position = corr * vec4(at / viewport * 2.0 - 1.0, z, 1.0);
    vColor = color;
    vAlong = dot(atB ? b : a, clip.xyz);
}
