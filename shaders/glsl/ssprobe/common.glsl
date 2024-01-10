struct RayPayload{
    vec3 radiance;
    vec3 worldpos;
    vec3 samplevec;
    vec3 brdf;
    float cosine;
    float pdf;
    uint seed;
    bool lightingflag;
    bool recursiveflag;
};

const float PI=3.1415926535897932384626433832795;