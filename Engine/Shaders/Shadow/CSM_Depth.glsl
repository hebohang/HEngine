#type vertex
#version 450 core

layout (location = 0) in vec3 a_Pos;

// Animation
layout (location = 6) in ivec4 a_BoneIds; 
layout (location = 7) in vec4 a_BoneWeights;

uniform mat4 model;

uniform bool u_Animated;

const int MAX_BONES = 100;
const int MAX_BONE_INFLUENCE = 4;
uniform mat4 finalBonesMatrices[MAX_BONES];

void main()
{
    if (u_Animated)
    {
	    mat4 boneTransform = finalBonesMatrices[a_BoneIds[0]] * a_BoneWeights[0];
        boneTransform += finalBonesMatrices[a_BoneIds[1]] * a_BoneWeights[1];
        boneTransform += finalBonesMatrices[a_BoneIds[2]] * a_BoneWeights[2];
        boneTransform += finalBonesMatrices[a_BoneIds[3]] * a_BoneWeights[3];

        vec4 localPosition = boneTransform * vec4(a_Pos, 1.0);

        gl_Position = model * localPosition;
	}
    else
    {
        gl_Position = model * vec4(a_Pos, 1.0);
    }
}

#type geometry
#version 450 core

layout(triangles, invocations = 5) in;
layout(triangle_strip, max_vertices = 3) out;

layout (std140, binding = 1) uniform LightSpaceMatrices
{
    mat4 lightSpaceMatrices[16];
};

void main()
{          
	for (int i = 0; i < 3; ++i)
	{
		gl_Position = lightSpaceMatrices[gl_InvocationID] * gl_in[i].gl_Position;
		gl_Layer = gl_InvocationID;
		EmitVertex();
	}
	EndPrimitive();
}  

#type fragment
#version 450 core

void main()
{             
}