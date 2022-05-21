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

mat3 GaussianBlur33 = mat3(
    0.057, 0.125, 0.057,
    0.125, 0.272, 0.125,
    0.057, 0.125, 0.057
);

void main()
{
    vec3 diffuse = texture(screenTexture, TexCoords.st).rgb;
	vec3 reCol = vec3(0., 0., 0.);
    for (int i=0; i<3; i++) 
	{
        for (int j=0; j<3; j++) 
		{
            vec3 texSample = texelFetch(screenTexture, ivec2(gl_FragCoord) + ivec2(i-1,j-1), 0).rgb;
            reCol += texSample * GaussianBlur33[i][j];
		}
	}
	
	FragColor = vec4(reCol, 1.0f);
} 
