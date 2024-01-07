struct RayPayload{
    vec3 radiance;
    vec3 worldpos;
    vec3 samplevec;
    vec3 attenuation;
    uint seed;
    bool recursiveflag;
    bool lightflag;
};

const float PI=3.1415926535897932384626433832795;