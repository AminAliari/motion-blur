#version 450 core

layout(location = 0) in vec4 vTexCoord;
layout(location = 0) out vec4 oColor;

layout (UPDATE_FREQ_NONE, binding = 0) uniform sampler uSampler;
layout (UPDATE_FREQ_NONE, binding = 1) uniform sampler uSamplerLinear;


layout (std140, UPDATE_FREQ_PER_FRAME, binding = 0) uniform envUniformBlock {
    uniform mat4 mView;
    uniform mat4 mProject;
    uniform mat4 mViewPrev;
    uniform mat4 mProjectPrev;
    uniform vec4 mLightDirection;
    uniform vec4 mLightColor;
};

layout (UPDATE_FREQ_PER_FRAME, binding = 1)  uniform texture2D colorTexture;
layout (UPDATE_FREQ_PER_FRAME, binding = 2)  uniform texture2D normTexture;
layout (UPDATE_FREQ_PER_FRAME, binding = 3)  uniform texture2D velocityTexture;
layout (UPDATE_FREQ_PER_FRAME, binding = 4)  uniform texture2D neighborTexture;

layout(row_major, push_constant) uniform cbRootConstants_Block {
    vec2  tileSize;
    float kFactor;
    float sFactor;
} cbRootConstants;

// Utils
float rand(vec2 co);

// Filters
float cone(float distance, float speed);
float cylinder(float distance, float speed);
float softDepthCompare(float za, float zb);

void main ()
{
    vec2  X = vTexCoord.xy;
    
    // Color
    vec3  norm         = normalize(texture(sampler2D(normTexture, uSampler), X).rgb);
    vec3  lightDir     = normalize(-mLightDirection.xyz);  
    float diff         = max(dot(norm, lightDir), 0.0);
    vec4  sampledX     = texture(sampler2D(colorTexture, uSampler), X).rgba;
    vec4  color 	   = vec4((mLightColor.xyz * diff * sampledX.xyz).rgb, 1.0);  
    color 	           = vec4((sampledX).rgb, 1.0);
    float zX           = sampledX.a;

    // Largest velocity in the neighborhood
    int k = int(cbRootConstants.kFactor);
    int s = int(cbRootConstants.sFactor);
    vec2 texelSize = cbRootConstants.tileSize;

    vec2  maxNeighbor    = texture(sampler2D(neighborTexture, uSamplerLinear), X).xy;
    float maxNeighborLen = length(maxNeighbor);

    if (maxNeighborLen <= 0.5) // Early out
    {
        oColor = color; // No blur
        return;
    }
    
    // Sample the current point
    float jitter = rand(X) * 2.0 - 1.0;
    vec2  vX    = texture(sampler2D(velocityTexture, uSamplerLinear), X).xy;
    float vXLen = length(vX) + 0.00000001;
    float weight = 1.0 / max(vXLen, 0.5);
    vec3  sum 	 = color.rgb * weight;

    // Take S − 1 additional neighbor samples
    for (float i = 0.0; i < s; i += 1.0)
    {
        // Choose evenly placed filter taps along ±~vN,
        // but jitter the whole filter to prevent ghosting
        float t = mix(-1.0, 1.0, (i + jitter + 1.0)/(s + 1.0));
        vec2 offset = maxNeighbor * t;
        offset = texelSize * vec2(offset.x, -offset.y);

        vec2  Y        =  X + offset; // Round to nearest
        vec4  sampledY = texture(sampler2D(colorTexture, uSampler), Y).rgba;
        vec3  cY       = sampledY.rgb;
        vec2  vY       = texture(sampler2D(velocityTexture, uSamplerLinear), Y).xy;
        float vYLen    = length(vY);
        float dist     = length(offset);
        float zY       = sampledY.a;

        // Fore- vs. background classification of Y relative to X
        float f = softDepthCompare(zX, zY);
        float b = softDepthCompare(zY, zX);

        // Case 1: Blurry Y in front of any X
        float aY = f * cone(dist, vYLen);
        
        // Case 2: Any Y behind blurry X; estimate background
        aY += b * cone(dist, vXLen);

        // Case 3: Simultaneously blurry X and Y
        aY += cylinder(dist, vYLen) * cylinder(dist, vXLen) * 2.0;

        // Accumulate
        weight += aY;
        sum += aY * cY.rgb;
    }

    oColor = vec4(sum/weight, 1.0);
}

// Utils - impl
float rand(vec2 co)
{
    return fract(sin(dot(co.xy, vec2(12.9898, 78.233)) * 43758.5453));
}

// Filters - impl
float cone(float distance, float speed)
{
    return clamp(1.0 - distance / speed, 0.0, 1.0);
}
float cylinder(float distance, float speed)
{
    return 1.0 - smoothstep(0.95 * speed, 1.05 * speed, distance);
}
float softDepthCompare(float za, float zb)
{
    return clamp(1.0 - (za - zb) / min(za, zb), 0.0, 1.0);
}