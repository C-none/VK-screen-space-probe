#version 460
#extension GL_EXT_ray_tracing:enable
#extension GL_GOOGLE_include_directive:require
#include "common.glsl"
layout(location=1)rayPayloadInEXT float dist;

void main()
{
	dist=-1.;
}