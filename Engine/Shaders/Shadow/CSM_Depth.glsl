#type vertex
#version 450 core

layout (location = 0) in vec3 a_Pos;

uniform mat4 model;

void main()
{
    gl_Position = model * vec4(a_Pos, 1.0);
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