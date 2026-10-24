#version 450
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
layout(location = 0) in vec3 in_pos;
layout(set = 0, binding = 0) uniform Ubo { mat4 mvp; vec4 tint; } ubo;
layout(push_constant) uniform Pc { vec4 a; vec4 b; vec4 c; vec4 d; } pc;
layout(location = 0) out vec2 v_uv;
void main() {
    gl_Position = ubo.mvp * vec4(in_pos, 1.0) + pc.a * 0.0;
    v_uv = in_pos.xy * 0.5 + 0.5 + pc.b.xy;
}
