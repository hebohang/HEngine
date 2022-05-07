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

layout(location = 0) out vec3 v_TexCoords;

void main()
{
	v_TexCoords = a_Pos;
	vec4 pos = u_ViewProjection * vec4(a_Pos, 1.0f);
	gl_Position = pos.xyww;
}

#type fragment
#version 450 core

layout(location = 0) in vec3 v_TexCoords;

layout(location = 0) out vec4 color;
layout(location = 1) out int o_EntityID;

layout(binding = 0) uniform samplerCube SkyBox;

void main()
{
	color = texture(SkyBox, v_TexCoords);
	o_EntityID = -1;
}
