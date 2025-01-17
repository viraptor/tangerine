--------------------------------------------------------------------------------

// Copyright 2023 Aeva Palecek
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


#if GL_ES

attribute vec4 LocalPosition;
attribute vec4 VertexColor;
uniform mat4 LocalToWorld;
uniform mat4 WorldToView;
uniform mat4 ViewToClip;

varying vec3 Color;

void main()
{
	vec4 ViewPosition = WorldToView * LocalToWorld * vec4(LocalPosition.xyz, 1.0);
	gl_Position = ViewToClip * ViewPosition;
	Color = VertexColor.rgb;
}

#else

layout(std140, binding = 0)
uniform ViewInfoBlock
{
	mat4 WorldToView;
	mat4 ViewToWorld;
	mat4 ViewToClip;
	mat4 ClipToView;
	vec4 CameraOrigin;
	vec4 ScreenSize;
	vec4 ModelMin;
	vec4 ModelMax;
	float CurrentTime;
	bool Perspective;
};


layout(std140, binding = 1)
uniform InstanceDataBlock
{
	mat4 LocalToWorld;
	mat4 WorldToLocal;
};


layout(std430, binding = 2)
restrict readonly buffer VertexIndexBlock
{
	int VertexIndices[];
};


layout(std430, binding = 3)
restrict readonly buffer VertexPositionBlock
{
	vec3 VertexPositions[];
};


layout(std430, binding = 4)
restrict readonly buffer VertexColorBlock
{
	vec3 VertexColors[];
};


out gl_PerVertex
{
	vec4 gl_Position;
	float gl_PointSize;
	float gl_ClipDistance[];
};

out vec3 VertexColor;
out vec3 Barycenter;


void main()
{
	int Index = VertexIndices[gl_VertexID];
	vec3 LocalPosition = VertexPositions[Index];
	vec4 ViewPosition = WorldToView * LocalToWorld * vec4(LocalPosition, 1.0);
	gl_Position = ViewToClip * ViewPosition;

	VertexColor = VertexColors[Index];

	Barycenter = vec3(0.0);
	Barycenter[gl_VertexID % 3] = 1.0;
}
#endif