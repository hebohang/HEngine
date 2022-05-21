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

float luminance(vec3 color)
{
	return 0.2125 * color.r + 0.7154 * color.g + 0.0721 * color.b;
}

void main()
{
    vec3 diffuse = texture(screenTexture, TexCoords).rgb;
    float grayVal = luminance(diffuse);
	FragColor = vec4(grayVal, grayVal, grayVal, 1.0);
} 
