#type vertex
#version 450 core

layout (location = 0) in vec2 a_Pos;
layout (location = 1) in vec2 a_TexCoords;

out vec2 TexCoords;

void main()
{
    TexCoords = a_TexCoords;
    gl_Position = vec4(a_Pos.x, a_Pos.y, 0.0, 1.0); 
}  

#type fragment
#version 450 core

out vec4 FragColor;

in vec2 TexCoords;

uniform sampler2D screenTexture;

uniform sampler2DArray depthMap;

void main()
{
    float depthValue = texture(depthMap, vec3(TexCoords, 1)).r;
    // FragColor = vec4(vec3(LinearizeDepth(depthValue) / far_plane), 1.0); // perspective
    FragColor = vec4(vec3(depthValue), 1.0); // orthographic
} 
