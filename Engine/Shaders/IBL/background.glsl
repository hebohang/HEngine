#type vertex
#version 450 core
layout(location = 0) in vec3 a_Pos;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec3 a_Tangent;
layout(location = 3) in vec2 a_TexCoord;
layout(location = 4) in int a_EntityID;

layout(std140, binding = 0) uniform Camera
{
	mat4 u_ViewProjection;
};

out vec3 WorldPos;

void main()
{
    WorldPos = a_Pos;

	vec4 clipPos = u_ViewProjection * vec4(WorldPos, 1.0);

	gl_Position = clipPos.xyww;
}

#type fragment
#version 450 core
out vec4 FragColor;
out int o_EntityID;
in vec3 WorldPos;

uniform samplerCube environmentMap;

void main()
{		
    vec3 envColor = textureLod(environmentMap, WorldPos, 0.0).rgb;
    
    // HDR tonemap and gamma correct
    envColor = envColor / (envColor + vec3(1.0));
    envColor = pow(envColor, vec3(1.0/2.2)); 
    
    FragColor = vec4(envColor, 1.0);

    o_EntityID = -1;
}
