#version 450
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// unwritten-inputs-test: reads locations 0..4; the vertex shader writes 0 and 1. Location 3 is
// read through a component access chain, location 4 is flat.
layout(location = 0) in vec4 in0;
layout(location = 1) in vec4 in1;
layout(location = 2) in vec4 in2;
layout(location = 3) in vec4 in3;
layout(location = 4) flat in vec4 in4;
layout(location = 0) out vec4 color;
void main() {
    color = in0 + in1 + in2 + vec4(in3.y) + in4;
}
