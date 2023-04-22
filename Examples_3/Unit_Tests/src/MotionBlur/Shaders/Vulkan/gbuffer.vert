#version 450 core

// Draw the objects, and calcs the velocity vectors

#define TOTAL_MODELS 2

layout(location = 0) in vec3 Position;
layout(location = 1) in vec3 Normal;
layout(location = 2) in vec2 TexCoord;

layout(location = 0) out vec4 vPosition;
layout(location = 1) out vec4 vPositionPrev;
layout(location = 2) out vec4 vNormal;
layout(location = 3) out vec4 vTexCoord;

layout (std140, UPDATE_FREQ_PER_FRAME, binding = 0) uniform envUniformBlock {
    uniform mat4 mView;
    uniform mat4 mProject;
    uniform mat4 mViewPrev;
    uniform mat4 mProjectPrev;
    uniform vec4 mLightDirection;
    uniform vec4 mLightColor;
};

struct ObjectInfo {
    mat4 mToWorldMat;
    mat4 mToWorldMatPrev;
};

layout (std140, UPDATE_FREQ_PER_FRAME, binding = 1) uniform objectsUniformBlock {
    ObjectInfo objects[TOTAL_MODELS];
};

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
    uint objectIndex = cbRootConstants.objectIndex;

    vTexCoord = vec4(TexCoord.xy, 0.0, 1.0);

    vPosition     = mProject * mView * objects[objectIndex].mToWorldMat * vec4(Position.xyz, 1.0); 
    vPositionPrev = mProjectPrev * mViewPrev * objects[objectIndex].mToWorldMatPrev * vec4(Position.xyz, 1.0);

    vNormal.xyz = (objects[objectIndex].mToWorldMat * vec4(Normal.xyz, 0.0)).xyz;

    gl_Position = vPosition;
}
