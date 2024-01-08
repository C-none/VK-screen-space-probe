/* Copyright (c) 2023, Sascha Willems
*
* SPDX-License-Identifier: MIT
*
*/

#version 460
#extension GL_EXT_ray_tracing:enable
#extension GL_GOOGLE_include_directive:require

#include "common.glsl"
layout(location=0)rayPayloadInEXT RayPayload rayPL;

void main()
{
	rayPL.radiance=vec3(0.);
	rayPL.attenuation=vec3(1.);
	rayPL.recursiveflag=false;
}