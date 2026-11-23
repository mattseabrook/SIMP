#version 450
layout(push_constant) uniform View { vec4 rect; vec2 surface; vec2 imageSize; int checker; } view;
layout(location=0) out vec2 uv;
const vec2 corners[6] = vec2[](vec2(0,0),vec2(1,0),vec2(0,1),vec2(0,1),vec2(1,0),vec2(1,1));
void main() {
    uv = corners[gl_VertexIndex];
    gl_Position = vec4((view.rect.xy + uv*view.rect.zw)/view.surface*2.0-1.0,0,1);
}
