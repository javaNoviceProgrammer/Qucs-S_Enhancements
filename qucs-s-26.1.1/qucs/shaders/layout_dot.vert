// A shape's dot: where the shape is under 1.5 pixels across, a device
// pixel at its middle (else none) - a quad a device pixel square, each dot
// an instance.
#version 440
layout(location = 0) in vec2 corner;   // ±0.5
layout(location = 1) in vec3 dot;      // x y (µm from the origin), its larger side (µm)
layout(std140, binding = 0) uniform Buf {
    mat4 corr;
    vec4 color;
    vec2 center;
    vec2 scale;
    vec2 nudge;
    vec2 viewport;
    float pixels;
    float cell;
    float patterned;
    float yUp;
};
void main()
{
    if (dot.z * pixels < 1.5) {
        vec2 clip = (dot.xy - center) * scale + nudge + corner * 2.0 / viewport;
        gl_Position = corr * vec4(clip, 0.0, 1.0);
    } else {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    }
}
