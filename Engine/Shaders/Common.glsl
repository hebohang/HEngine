#type vertex
#version 450 core

layout(location = 0) in vec3 a_Pos;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec3 a_Tangent;
layout(location = 3) in vec2 a_TexCoord;
layout(location = 4) in int a_EntityID;

uniform mat4 u_ViewProjection;
uniform mat4 u_Model;

out vec3 v_Normal;
out vec3 v_Tangent;
out vec2 v_TexCoord;
out flat int v_EntityID;

void main()
{
	v_Normal = a_Normal;
	v_Tangent = a_Tangent;
	v_TexCoord = a_TexCoord;
	v_EntityID = a_EntityID;
	gl_Position = u_ViewProjection * u_Model * vec4(a_Pos, 1.0);
}

#type fragment
#version 450 core

in vec3 v_Normal;
in vec3 v_Tangent;
in vec2 v_TexCoord;
in flat int v_EntityID;

layout(location = 0) out vec4 color;
layout(location = 1) out int color2;


void main()
{
	color = vec4(1.0f, 0.2f, 0.3f, 1.0f);
	color2 = v_EntityID;
}