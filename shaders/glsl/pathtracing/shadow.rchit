#version 460
#extension GL_EXT_ray_tracing:require
#extension GL_GOOGLE_include_directive:require
#include "common.glsl"
layout(location=1)rayPayloadInEXT float dist;

void main()
{
	dist=gl_HitTEXT;
}