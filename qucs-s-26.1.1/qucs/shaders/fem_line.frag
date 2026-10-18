#version 440
layout(location = 0) flat in vec2 a;
layout(location = 1) flat in vec2 b;
layout(location = 2) flat in float halfWidth;
layout(location = 3) flat in vec4 vColor;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform Buf {
    mat4 corr;
    vec4 uniformColor;
    vec2 center;
    vec2 scale;
    vec2 viewport;
    float ratio;
    float roundCaps;
    float useUniform;
    float uniformWidth;
    float yUp;
};
void main()
{
    vec2 p = yUp > 0.5 ? gl_FragCoord.xy : vec2(gl_FragCoord.x, viewport.y - gl_FragCoord.y);
    vec2 d = b - a;
    float len = length(d);
    vec2 t = len > 1e-6 ? d / len : vec2(1.0, 0.0);
    float along = dot(p - a, t);
    float c;
    if (roundCaps > 0.5) {
        c = clamp(halfWidth + 0.5 - length(p - (a + t * clamp(along, 0.0, len))), 0.0, 1.0);
    } else {
        float across = abs(dot(p - a, vec2(-t.y, t.x)));
        c = clamp(halfWidth + 0.5 - across, 0.0, 1.0) * clamp(along + 0.5, 0.0, 1.0) * clamp(len - along + 0.5, 0.0, 1.0);
    }
    if (c <= 0.0) discard;
    fragColor = vec4(vColor.rgb, vColor.a * c);
}
