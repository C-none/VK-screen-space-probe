/* Copyright (c) 2023, Sascha Willems
*
* SPDX-License-Identifier: MIT
*
*/

#version 460

#extension GL_EXT_ray_tracing:require
#extension GL_GOOGLE_include_directive:require
#extension GL_EXT_nonuniform_qualifier:require
#extension GL_EXT_buffer_reference2:require
#extension GL_EXT_scalar_block_layout:require
#extension GL_EXT_shader_explicit_arithmetic_types_int64:require
#include "common.glsl"

layout(location=0)rayPayloadInEXT RayPayload rayPL;
layout(location=1)rayPayloadEXT float dist;
hitAttributeEXT vec2 attribs;

layout(binding=0,set=0)uniform accelerationStructureEXT topLevelAS;
layout(binding=2,set=0)uniform UBO
{
	mat4 viewInverse;
	mat4 projInverse;
	uint frame;
	uint randomSeed;
	uint recuresiveDepth;
	uint sampleDimenson;
	uint lightCount;
	bool enableDirectLighting;
}ubo;

struct GeometryNode{
	uint64_t vertexBufferDeviceAddress;
	uint64_t indexBufferDeviceAddress;
	int textureIndexBaseColor;
	int textureIndexNormal;
};
layout(binding=4,set=0)buffer GeometryNodes{GeometryNode nodes[];}geometryNodes;

layout(binding=5,set=0)uniform sampler2D textures[];

struct Light{
	vec4 position;
	vec3 color;
	float intensity;
};
const int maxLights=4;
layout(binding=3,set=0)uniform LightBlock{
	Light light[maxLights];
}lights;

#include "bufferreferences.glsl"
#include "geometrytypes.glsl"
#include "random.glsl"

bool check_visibility(vec3 lightvec){
	traceRayEXT(topLevelAS,gl_RayFlagsNoneEXT,0xFF,1,0,1,rayPL.worldpos,.001,normalize(lightvec),10000.,1);
	return dist<0||dist>length(lightvec);
}

float pow5(float x){
	float x2=x;
	return x2*x2*x;
}

vec3 diffuse(vec3 basecolor,float roughness,float NoV,float NoL,float VoH){
	float FD90=.5+2.*VoH*VoH*roughness;
	float FdV=1.+(FD90-1.)*pow5(1.-NoV);
	float FdL=1.+(FD90-1.)*pow5(1.-NoL);
	return basecolor*((1./PI)*FdV*FdL);
}

float roughness=1.;

Triangle tri;
GeometryNode geometryNode;
vec3 compute_albedo(vec3 l){
	// incident: vector from hit point to light source
	l=normalize(l);
	vec3 v=-normalize(gl_WorldRayDirectionEXT);
	vec3 h=normalize(l+v);
	float NoL=dot(tri.normal,l);// theta l
	float NoV=dot(tri.normal,v);// theta v
	float NoH=dot(tri.normal,h);// theta h
	float VoH=dot(v,h);// theta h
	
	vec3 baseColor=texture(textures[nonuniformEXT(geometryNode.textureIndexBaseColor)],tri.uv).rgb;
	
	// diffuse
	vec3 diffuse=diffuse(baseColor,roughness,NoV,NoL,VoH);
	
	return diffuse;
	//	return basecolor;
}
mat3 TBN;

vec3 generate_hemisphere(out float pdf){
	float a=2.*PI*rnd(rayPL.seed);
	float cosb=sqrt(rnd(rayPL.seed));
	float sinb=sqrt(1.-cosb*cosb);
	vec3 samplevec=vec3(cos(a)*sinb,sin(a)*sinb,cosb);
	pdf=cosb/PI;
	return normalize(TBN*samplevec);
}

void main()
{
	tri=unpackTriangle(gl_PrimitiveID);
	rayPL.worldpos=gl_WorldRayOriginEXT+gl_WorldRayDirectionEXT*gl_HitTEXT;
	
	geometryNode=geometryNodes.nodes[gl_GeometryIndexEXT];
	
	vec3 albedo=texture(textures[nonuniformEXT(geometryNode.textureIndexBaseColor)],tri.uv).rgb;
	// compute t b n
	vec3 N=normalize(tri.normal);
	vec3 T=normalize(tri.tangent.xyz);
	vec3 B=normalize(cross(N,T)*tri.tangent.w);
	TBN=mat3(T,B,N);
	
	vec3 worldnormal=normalize(TBN*normalize(texture(textures[nonuniformEXT(geometryNode.textureIndexNormal)],tri.uv).rgb*2.-vec3(1.)));
	if(dot(worldnormal,gl_WorldRayDirectionEXT)>0.)worldnormal*=-1.;
	
	rayPL.radiance=vec3(0.);
	vec3 direct_lighting=vec3(0.);
	//	if(rayPL.lightflag){
		// direct lighting
		for(int i=0;i<ubo.lightCount;i++){
			// check visibility
			vec3 lightvec=lights.light[i].position.xyz-rayPL.worldpos;
			if(dot(worldnormal,lightvec)>=0.&&check_visibility(lightvec)){
				float lightDistance=length(lightvec);
				float attenuation=1./(lightDistance*lightDistance);
				//				float attenuation=1./lightDistance;
				float NdotL=max(dot(worldnormal,normalize(lightvec)),0.);
				direct_lighting+=lights.light[i].color*NdotL*lights.light[i].intensity*attenuation*compute_albedo(lightvec);
			}
		}
	//	}
	//	rayPL.radiance=compute_albedo(-gl_WorldRayDirectionEXT);
	//	rayPL.radiance=vec3(lights.light.intensity/4.);
	rayPL.radiance=direct_lighting;
	//		rayPL.radiance=worldnormal;
	// indirect lighting
	// test Russian Roulette
	//	const float p=1/8.;
	//	if(rnd(rayPL.seed)<p){
		//		rayPL.recursiveflag=false;
	//	}else{
		rayPL.recursiveflag=true;
		// sample hemisphere
		float pdf;
		rayPL.samplevec=generate_hemisphere(pdf);
		rayPL.attenuation=compute_albedo(rayPL.samplevec)*dot(worldnormal,rayPL.samplevec)/pdf;
	//	}
	
	// Trace shadow ray and offset indices to match shadow hit/miss shader group indices
	//	traceRayEXT(topLevelAS,gl_RayFlagsTerminateOnFirstHitEXT|gl_RayFlagsOpaqueEXT|gl_RayFlagsSkipClosestHitShaderEXT,0xFF,0,0,1,origin,tmin,lightVector,tmax,2);
	
}
