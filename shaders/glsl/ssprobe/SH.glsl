//  for the storage image
// .-------.-> x
// |       |
// |       |
// '-------'
// y
// for SH base function refered to https://github.com/EpicGames/UnrealEngine/blob/072300df18a94f18077ca20a14224b5d99fee872/Engine/Shaders/Private/SHCommon.ush#L210
// L range from 1 to 3 and M range from -(L-1) to L-1, which is consistent with UE5
// M=             -2                  -1                        0                      1                       2
// L = 1                                                  0.5*sqrt(1/pi)
// L = 2                         -sqrt(3/(4pi))*y         sqrt(3/(4pi))*z       -sqrt(3/(4pi))*x
// L = 3  0.5*sqrt(15/pi)*x*y  -0.5*sqrt(15/pi)*y*z  0.25*sqrt(5/pi)*(3z^2-1)  -0.5*sqrt(15/pi)*x*z  0.25*sqrt(15/pi)*(x^2-y^2)
uint getIndex(int l,int m){
    return l*(l-1)+m;
}

void update(inout vec3 SH[9],vec3 direction,vec3 value){
    direction=normalize(direction);
    float x=direction.x,y=direction.y,z=direction.z;
    SH[getIndex(1,0)]+=value*.282095;
    SH[getIndex(2,-1)]+=-value*.488603*y;
    SH[getIndex(2,1)]+=value*.488603*z;
    SH[getIndex(2,0)]+=-value*.488603*x;
    SH[getIndex(3,-2)]+=value*1.092548*x*y;
    SH[getIndex(3,-1)]+=-value*-1.092548*y*z;
    SH[getIndex(3,0)]+=value*.315392*(3.*z*z-1.);
    SH[getIndex(3,1)]+=-value*-1.092548*x*z;
    SH[getIndex(3,2)]+=value*.546274*(x*x-y*y);
}
