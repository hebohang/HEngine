#type vertex
#version 450 core

layout (location = 0) in vec2 a_Pos;
layout (location = 1) in vec2 a_TexCoords;

out vec2 TexCoords;

void main()
{
    TexCoords = a_TexCoords;
    gl_Position = vec4(a_Pos.x, a_Pos.y, 0.0, 1.0); 
}  

#type fragment
#version 450 core

out vec4 FragColor;

in vec2 TexCoords;

uniform sampler2D screenTexture;

vec3 GetStepVal(vec3 value)
{
    value = value * 5;
    value.r = float(floor(value.r)) / 5;
    value.g = float(floor(value.g)) / 5;
    value.b = float(floor(value.b)) / 5;
    return value;
}

void main()
{
    vec3 diffuse = texture(screenTexture, TexCoords).rgb;
	
	FragColor = vec4(GetStepVal(diffuse), 1.0);
} 
