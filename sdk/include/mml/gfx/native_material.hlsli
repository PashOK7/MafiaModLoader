// Verified LS3D encoded fixed-stage material subset. Inputs are native interpolated prelit diffuse and
// texture texels, NOT linear light. Convert only the FINAL stage RGB; alpha remains linear coverage.
// Unsupported operations/arguments are explicit unavailable material, never an invented white fallback.
#ifndef MML_NATIVE_MATERIAL_HLSLI
#define MML_NATIVE_MATERIAL_HLSLI
float4 MmlNativeArgb(uint c){return float4((c>>16)&255,(c>>8)&255,c&255,c>>24)/255.0;}
float4 MmlNativeArgument(uint arg,float4 textureColor,float4 current,float4 diffuse,float4 factor,inout bool valid){
    uint select=arg&15u;float4 value=0;
    if(select==0)value=diffuse;else if(select==1)value=current;else if(select==2)value=textureColor;
    else if(select==3)value=factor;else valid=false;
    if((arg&16u)!=0)value=1-value;if((arg&32u)!=0)value=value.aaaa;
    if((arg&~63u)!=0)valid=false;return value;
}
float4 MmlNativeOperation(uint op,float4 a,float4 b,float4 factor,inout bool valid){
    if(op==2)return a;if(op==3)return b;if(op==4)return a*b;if(op==5)return saturate(2*a*b);
    if(op==7)return saturate(a+b);if(op==14)return lerp(b,a,factor.a);
    valid=false;return 0;
}
// rgb/alpha = op,arg0,arg1,arg2. Native observed draws do not use ARG0, RESULTARG/TEMP or UV transforms.
float4 MmlNativeStage(uint4 rgb,uint4 alpha,float4 tex,float4 current,float4 diffuse,float4 factor,bool hasTexture,inout bool valid){
    if(rgb.x==1||!hasTexture)return current;
    float4 a=MmlNativeArgument(rgb.z,tex,current,diffuse,factor,valid),b=MmlNativeArgument(rgb.w,tex,current,diffuse,factor,valid);
    float3 color=MmlNativeOperation(rgb.x,a,b,factor,valid).rgb;
    a=MmlNativeArgument(alpha.z,tex,current,diffuse,factor,valid);b=MmlNativeArgument(alpha.w,tex,current,diffuse,factor,valid);
    float opacity=MmlNativeOperation(alpha.x,a,b,factor,valid).a;
    return saturate(float4(color,opacity));
}
float3 MmlNativeLinear(float3 encoded){encoded=saturate(encoded);return float3(encoded.r<=.04045?encoded.r/12.92:pow((encoded.r+.055)/1.055,2.4),
    encoded.g<=.04045?encoded.g/12.92:pow((encoded.g+.055)/1.055,2.4),encoded.b<=.04045?encoded.b/12.92:pow((encoded.b+.055)/1.055,2.4));}
#endif
