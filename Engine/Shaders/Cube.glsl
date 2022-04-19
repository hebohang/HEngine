#type vertex
#version 450 core

layout(location = 0) in vec3 a_Pos;
layout(location = 1) in vec2 a_Coord;
layout(location = 2) in vec3 a_Normal;

uniform mat4 u_ViewProjection;
uniform mat4 u_Model;

out vec2 v_Coord;
out vec3 v_Normal;

void main()
{
	v_Coord = a_Coord;
	v_Normal = a_Normal;
	gl_Position = u_ViewProjection * u_Model * vec4(a_Pos, 1.0);
}

#type fragment
#version 450 core

in vec2 v_Coord;
in vec3 v_Normal;
out vec4 color;

void main()
{
	color = vec4(1.0f, 0.2f, 0.3f, 1.0f);
}