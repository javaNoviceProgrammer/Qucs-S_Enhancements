// Multiphysics 3D's triangles: an orthographic camera, each lit by a light
// at the eye on both sides (a face between two domains is seen from either)
// - a field's colours lit less, its values what they say -, the Cut's
// half-space their fragments' to discard. As the CPU's (view3d.cpp).
#version 440
layout(location = 0) in vec3 position;   // relative to the scene's origin
layout(location = 1) in vec4 normal;     // xyz the face's, w 1 a field's colours
layout(location = 2) in vec4 color;
layout(location = 0) out vec4 vColor;
layout(location = 1) out float vAlong;
layout(std140, binding = 0) uniform Buf {
    mat4 corr;
    vec4 right;
    vec4 up;
    vec4 toward;
    vec4 center;       // xyz, relative to the origin
    vec4 clip;         // xyz the Cut's axis, w where (relative)
    vec2 scale;        // clip space a unit, across and up
    vec2 viewport;     // device pixels
    float depthRange;  // the units from the centre to the clip space's depth ±1
    float bias;        // units toward the eye: lines' and dots'
    float clipOn;
    float pixel;       // units a device pixel
};
void main()
{
    vec3 d = position - center.xyz;
    gl_Position = corr * vec4(dot(d, right.xyz) * scale.x, dot(d, up.xyz) * scale.y, -dot(d, toward.xyz) / depthRange, 1.0);
    float facing = abs(dot(normal.xyz, toward.xyz));
    float light = normal.w > 0.5 ? 0.72 + 0.28 * facing : 0.38 + 0.62 * facing;
    vColor = vec4(min(color.rgb * light, vec3(1.0)), 1.0);
    vAlong = dot(position, clip.xyz);
}
