#version 450 core

#define TOTAL_IMAGES 84
#define albedoMap ((cbRootConstants.textureIds >> 0) & 0xFF)

layout(location = 0) in vec4 vPosition;
layout(location = 1) in vec4 vPositionPrev;
layout(location = 2) in vec4 vNormal;
layout(location = 3) in vec4 vTexCoord;

layout(location = 0) out vec4 oColor; // rgb: albedo, a: depth
layout(location = 1) out vec4 oNormal;
layout(location = 2) out vec4 oVelocity;

layout (UPDATE_FREQ_NONE, binding = 0) uniform sampler   uSampler;
layout (UPDATE_FREQ_NONE, binding = 1) uniform texture2D textureMaps[TOTAL_IMAGES];

layout(row_major, push_constant) uniform cbRootConstants_Block {
    vec2  viewport;
    float kFactor;
    uint  textureIds;
    uint  objectIndex;
    float exposure;
    float deltaTime;
} cbRootConstants;

void main ()
{
    oColor.rgb = texture(sampler2D(textureMaps[albedoMap], uSampler), vTexCoord.xy).rgb;
    oColor.a   = gl_FragCoord.z;

    oNormal = vec4(vNormal.xyz, 1.0);

    vec2 a = (vPosition.xy / vPosition.w);
    vec2 b = (vPositionPrev.xy / vPositionPrev.w);
    vec2 motion = (a - b) * (cbRootConstants.exposure / cbRootConstants.deltaTime);
    motion *= cbRootConstants.viewport * 0.5;
    motion /= max(1.0, length(motion) / cbRootConstants.kFactor);
    
    // Encoding the half velocity
    oVelocity = vec4(motion * 0.5, 0.0, 1.0);
}
