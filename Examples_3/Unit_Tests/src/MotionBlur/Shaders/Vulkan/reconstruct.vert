#version 450 core

// Reconstruction filter

layout(location = 0) out vec4 vTexCoord;

void main ()
{
    vTexCoord   = vec4((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2, 0, 0);
    gl_Position = vec4(vTexCoord.xy * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);
}
