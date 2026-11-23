#version 450
layout(set=0,binding=0) uniform sampler2D cel;
layout(push_constant) uniform View { vec4 rect; vec2 surface; vec2 imageSize; int checker; } view;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
void main() {
    if (view.checker != 0) {
        ivec2 tile = ivec2(floor(uv*view.imageSize/8.0));
        color = vec4(vec3(((tile.x+tile.y)&1) == 0 ? 0.20 : 0.27),1.0);
    } else color = texture(cel,uv);
}
