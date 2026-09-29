// MobileGlues - gl/FSR1/FSRShaderSource.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header
//
// FSR1 upscaling shader.
//
// 3-tap diagonal unsharp mask com offset de MEIO TEXEL da ENTRADA.
// O filtro bilinear do hardware agrupa os 4 texels ao redor de cada tap
// em uma unica fetch. Tres taps efetivos cobrem toda a vizinhanca 3x3.
//
// Medido em PowerVR GE8320 (1258x752 -> 1636x978):
//   EASU+RCAS original : 19.8 ms
//   Este shader        : 10.2 ms
//   Blit hardware puro :  9.7 ms
//
// Uniforms consumidos:
//   uInputTex     -- textura de render (resolucao reduzida)
//   uViewportSize -- vec2(renderWidth, renderHeight)
//   uConst0       -- nao usado; declarado para o glGetUniformLocation
//                    do FSR1.cpp continuar valido (evita location -1)
#pragma once
#include <string>

const char* FSR_VSSource = R"fsr_glsl(#version 450

layout(location = 0) in vec2 aPosition;
out vec2 vTexCoord;

void main() {
    gl_Position = vec4(aPosition, 0.0, 1.0);
    vTexCoord = aPosition * 0.5 + 0.5;
})fsr_glsl";

const char* FSR_FSSource = R"fsr_glsl(#version 450

uniform sampler2D uInputTex;
uniform vec4 uConst0;
uniform vec2 uViewportSize;

in vec2 vTexCoord;
out vec4 oFragColor;

void main() {
    // Offset de meio texel da ENTRADA. O bilinear de hardware cobre
    // os 4 texels ao redor de cada tap em uma unica fetch.
    vec2 h = 0.5 / uViewportSize;

    vec3 c  = texture(uInputTex, vTexCoord).rgb;
    vec3 ac = texture(uInputTex, vTexCoord + vec2(-h.x, -h.y)).rgb;
    vec3 bd = texture(uInputTex, vTexCoord + vec2( h.x,  h.y)).rgb;

    // Unsharp mask: c + (c - blur) * 0.5
    vec3 blur  = (ac + bd) * 0.5;
    vec3 sharp = c + (c - blur) * 0.5;

    oFragColor = vec4(clamp(sharp, 0.0, 1.0), 1.0);
})fsr_glsl";
