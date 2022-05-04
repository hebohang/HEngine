#type vertex
#version 450 core

layout(location = 0) in vec3 a_Pos;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord;
layout(location = 3) in int a_EntityID;

layout(std140, binding = 0) uniform UBO{
	mat4 u_Model;
	mat4 u_ViewProjection;	
} ubo;

layout(location = 0) out vec3 v_TexCoords;

void main()
{
	v_TexCoords = a_Pos;
	vec4 pos = ubo.u_ViewProjection * vec4(a_Pos, 1.0f);
	gl_Position = pos.xyww;
}

#type fragment
#version 450 core

layout(location = 0) in vec3 v_TexCoords;

layout(location = 0) out vec4 color;

layout (binding = 0) uniform samplerCube SkyBox;

void main()
{
	color = texture(SkyBox, v_TexCoords);
}
