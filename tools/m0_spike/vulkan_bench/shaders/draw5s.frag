#version 450
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
layout(location = 0) in vec2 v_uv;
layout(set = 0, binding = 0) uniform Ubo { mat4 mvp; vec4 tint; } ubo;
layout(set = 0, binding = 1) uniform texture2D tex;
layout(set = 0, binding = 2) uniform sampler smp;
layout(set = 0, binding = 3, std430) readonly buffer Sb { vec4 v[]; } sb;
layout(push_constant) uniform Pc { vec4 a; vec4 b; vec4 c; vec4 d; } pc;
layout(location = 0) out vec4 o0;
layout(location = 1) out vec4 o1;
layout(location = 2) out vec4 o2;
layout(location = 3) out vec4 o3;
layout(location = 4) out vec4 o4;
void main() {
    vec4 c = texture(sampler2D(tex, smp), v_uv) * ubo.tint + pc.c;
    c += sb.v[int(pc.d.x)];
    o0 = c * float(0 + 1);
    o1 = c * float(1 + 1);
    o2 = c * float(2 + 1);
    o3 = c * float(3 + 1);
    o4 = c * float(4 + 1);
}
