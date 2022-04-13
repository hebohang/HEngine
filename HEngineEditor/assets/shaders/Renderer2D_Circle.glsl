// --------------------------
// - HEngine 2D -
// Renderer2D Circle Shader
// --------------------------

#type vertex
#version 450

layout(location = 0) in vec3 a_WorldPosition;
layout(location = 1) in vec3 a_LocalPosition;
layout(location = 2) in vec4 a_Color;
layout(location = 3) in float a_Thickness;
layout(location = 4) in float a_Fade;
layout(location = 5) in int a_EntityID;

uniform mat4 u_ViewProjection;

struct VertexOutput
{
	vec3 LocalPosition;
	vec4 Color;
	float Thickness;
	float Fade;
};

out VertexOutput Output;
out flat int v_EntityID;

void main()
{
	Output.LocalPosition = a_LocalPosition;
	Output.Color = a_Color;
	Output.Thickness = a_Thickness;
	Output.Fade = a_Fade;

	v_EntityID = a_EntityID;

	gl_Position = u_ViewProjection * vec4(a_WorldPosition, 1.0);
}

#type fragment
#version 450

layout(location = 0) out vec4 o_Color;
layout(location = 1) out int o_EntityID;

struct VertexOutput
{
	vec3 LocalPosition;
	vec4 Color;
	float Thickness;
	float Fade;
};

in VertexOutput Output;
in flat int v_EntityID;

void main()
{
	float distance = 1.0 - length(Output.LocalPosition);
	float circle = smoothstep(0.0, Output.Fade, distance);
	circle *= smoothstep(Output.Thickness + Output.Fade, Output.Thickness, distance);
	if (circle < 1.0)
		discard;

	o_Color = Output.Color;
	o_Color.a *= circle;

	o_EntityID = v_EntityID;
}
