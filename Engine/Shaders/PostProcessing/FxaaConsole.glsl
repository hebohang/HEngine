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

uniform vec2 TexelSize;

const float _ContrastThreshold = 0.2;
const float _RelativeThreshold = 0.2;
const float _Scale = 0.5;
const float _Sharpness = 8;

float luminance(vec3 color)
{
	return 0.2125 * color.r + 0.7154 * color.g + 0.0721 * color.b;
}

void main()
{
    vec3 diffuse = texture(screenTexture, TexCoords.st).rgb;
	float lumaM = luminance(diffuse);
	float lumaNW = luminance(texelFetch(screenTexture, ivec2(gl_FragCoord) + ivec2(-1,1),  0).rgb);
	float lumaNE = luminance(texelFetch(screenTexture, ivec2(gl_FragCoord) + ivec2(1,1),   0).rgb);
	float lumaSW = luminance(texelFetch(screenTexture, ivec2(gl_FragCoord) + ivec2(-1,-1), 0).rgb);
	float lumaSE = luminance(texelFetch(screenTexture, ivec2(gl_FragCoord) + ivec2(1,-1),  0).rgb);

	float MaxLuma = max(max(lumaNW, lumaNE), max(lumaSW, lumaSE));
	float MinLuma = min(min(lumaNW, lumaNE), min(lumaNW, lumaNE));
	float Contrast = max(MaxLuma, lumaM) -  min(MinLuma, lumaM);

	if(Contrast < max(_ContrastThreshold, MaxLuma * _RelativeThreshold))
	{
		FragColor = vec4(diffuse, 1.0);
		return;
	}

	lumaNE += 0.001;

	vec2 Dir;
	Dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
	Dir.y = ((lumaNE + lumaSE) - (lumaNW + lumaSW));
	vec2 Dir1 = normalize(Dir);

	// vec2 Dir1 = Dir * TexelSize.xy * _Scale;

	vec4 N1 = texelFetch(screenTexture, ivec2(gl_FragCoord) - ivec2(ceil(Dir1)),  0);
	vec4 P1 = texelFetch(screenTexture, ivec2(gl_FragCoord) + ivec2(ceil(Dir1)),  0);
	vec4 Result = (N1 + P1) * 0.5;

	float DirAbsMinTimesC = min(abs(Dir1.x), abs(Dir1.y)) * _Sharpness;
	vec2 Dir2 = clamp(Dir1.xy / DirAbsMinTimesC, -2.0, 2.0) * 2;
	vec4 N2 = texelFetch(screenTexture, ivec2(gl_FragCoord) - ivec2(ceil(Dir2)),  0);
	vec4 P2 = texelFetch(screenTexture, ivec2(gl_FragCoord) + ivec2(ceil(Dir2)),  0);
	vec4 Result2 = Result * 0.5f + (N2 + P2) * 0.25f;
	
	float NewLum = luminance(Result2.rgb);
	if((NewLum >= MinLuma) && (NewLum <= MaxLuma)) 
	{
		Result = Result2;
	}
	FragColor = Result;
} 
