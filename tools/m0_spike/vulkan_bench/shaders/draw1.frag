#version 450
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
layout(location = 0) in vec2 v_uv;
layout(set = 0, binding = 0) uniform Ubo { mat4 mvp; vec4 tint; } ubo;
layout(set = 0, binding = 1) uniform texture2D tex;
layout(set = 0, binding = 2) uniform sampler smp;
layout(push_constant) uniform Pc { vec4 a; vec4 b; vec4 c; vec4 d; } pc;
layout(location = 0) out vec4 o0;
void main() {
    vec4 c = texture(sampler2D(tex, smp), v_uv) * ubo.tint + pc.c;
    o0 = c * float(0 + 1);
}
