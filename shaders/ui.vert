#version 450
layout(location = 0) in vec4 rect;
layout(location = 1) in vec4 color;
layout(location = 0) out vec4 tint;
layout(push_constant) uniform View { vec2 extent; } view;
void main() {
    const vec2 corners[6] = vec2[6](vec2(0,0), vec2(1,0), vec2(0,1),
                                   vec2(0,1), vec2(1,0), vec2(1,1));
    vec2 pixel = rect.xy + corners[gl_VertexIndex] * rect.zw;
    gl_Position = vec4(pixel / view.extent * 2.0 - 1.0, 0.0, 1.0);
    tint = color;
}
