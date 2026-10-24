#version 450
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// unwritten-inputs-test: a full-screen triangle that writes locations 0 and 1 only.
layout(location = 0) out vec4 out0;
layout(location = 1) out vec4 out1;
void main() {
    const vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    out0 = vec4(p, 0.0, 1.0);
    out1 = vec4(1.0, p, 1.0);
}
