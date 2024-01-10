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
	rayPL.radiance=vec3((gl_WorldRayDirectionEXT.z+1.)/2.);
	rayPL.brdf=vec3(1.);
	rayPL.cosine=0.;
	rayPL.pdf=1.;
	rayPL.recursiveflag=false;
}