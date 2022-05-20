#type vertex
#version 450 core

layout(location = 0) in vec3 a_Pos;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord;
layout(location = 3) in vec3 a_Tangent;
layout(location = 4) in vec3 a_Bitangent;
layout(location = 5) in int a_EntityID;
// Animation
layout(location = 6) in ivec4 a_BoneIds; 
layout(location = 7) in vec4 a_BoneWeights;

layout(std140, binding = 0) uniform Camera
{
	mat4 u_ViewProjection;
};

uniform mat4 model;

const int MAX_BONES = 100;
const int MAX_BONE_INFLUENCE = 4;
uniform mat4 finalBonesMatrices[MAX_BONES];

void main()
{
    mat4 boneTransform = finalBonesMatrices[a_BoneIds[0]] * a_BoneWeights[0];
    boneTransform += finalBonesMatrices[a_BoneIds[1]] * a_BoneWeights[1];
    boneTransform += finalBonesMatrices[a_BoneIds[2]] * a_BoneWeights[2];
    boneTransform += finalBonesMatrices[a_BoneIds[3]] * a_BoneWeights[3];

    vec4 localPosition = boneTransform * vec4(a_Pos, 1.0);

	vec4 FragPos = model * localPosition;
    gl_Position = u_ViewProjection * vec4(FragPos.rgb + a_Normal * 0.1, FragPos.a);
}

#type fragment
#version 450 core
out vec4 FragColor;

void main()
{
    FragColor = vec4(1.0, 0.6, 0.0, 1.0);
}