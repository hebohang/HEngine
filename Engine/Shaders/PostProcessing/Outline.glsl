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

// from https://computergraphics.stackexchange.com/questions/3646/opengl-glsl-sobel-edge-detection-filter
// Sobel
mat3 sx = mat3( 
    -1.0, 0.0, 1.0, 
    -2.0, 0.0, 2.0, 
    -1.0, 0.0, 1.0 
);
mat3 sy = mat3( 
    -1.0, -2.0, -1.0, 
    0.0, 0.0, 0.0, 
    1.0, 2.0, 1.0 
);

void main()
{
    vec3 diffuse = texture(screenTexture, TexCoords.st).rgb;
    mat3 I;
    for (int i=0; i<3; i++) 
	{
        for (int j=0; j<3; j++) 
		{
            vec3 texSample = texelFetch(screenTexture, ivec2(gl_FragCoord) + ivec2(i-1,j-1), 0).rgb;
            I[i][j] = length(texSample);
		}
	}

	float gx = dot(sx[0], I[0]) + dot(sx[1], I[1]) + dot(sx[2], I[2]); 
	float gy = dot(sy[0], I[0]) + dot(sy[1], I[1]) + dot(sy[2], I[2]);

	float g = sqrt(pow(gx, 2.0)+pow(gy, 2.0));
	g = smoothstep(0.6, 0.8, g);
	
	FragColor = vec4(diffuse - vec3(g), 1.0);
	
	// if you want to set the edge color, you can set as:
	// vec3 edgeColor = vec3(1., 0., 0.2);
    // FragColor = vec4(mix(diffuse, edgeColor, g), 1.);
} 
