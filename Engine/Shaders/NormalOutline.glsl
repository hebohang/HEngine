#type vertex
#version 450 core

layout(location = 0) in vec3 a_Pos;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord;
layout(location = 3) in vec3 a_Tangent;
layout(location = 4) in vec3 a_Bitangent;
layout(location = 5) in int a_EntityID;

layout(std140, binding = 0) uniform Camera
{
	mat4 u_ViewProjection;
};

uniform mat4 model;

void main()
{
    vec3 FragPos = (model * vec4(a_Pos, 1.0)).xyz;
    gl_Position = u_ViewProjection * vec4(FragPos + a_Normal * 0.1, 1.0);
}

#type fragment
#version 450 core
out vec4 FragColor;

void main()
{
    FragColor = vec4(1.0, 0.6, 0.0, 1.0);
}