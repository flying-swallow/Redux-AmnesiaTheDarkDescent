#version 440
#extension GL_GOOGLE_include_directive : require
/// Copyright © 2009-2020 Frictional Games
/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
#include "gui.resource.glsl"

layout(location = 0) out vec2 v_texcoord;
layout(location = 1) out vec4 v_color;
layout(location = 2) out vec4 v_position;

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec2 a_texcoord;
layout(location = 2) in vec4 a_color; 

void main(void)
{
    v_color = a_color;
    v_texcoord = a_texcoord;
    // v_position carries the screen-space (pre-MVP) position because the clip
    // planes in pass.clipPlane are computed in screen-space (see cGuiSet::Render).
    v_position = vec4(a_position, 1.0);
    gl_Position = pass.mvp * vec4(a_position, 1.0);
}
