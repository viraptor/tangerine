
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

#include "sdf_rendering.h"
#include "sdf_model.h"
#include "profiling.h"
#include <iostream>


struct TransformUpload
{
	glm::mat4 LocalToWorld;
	glm::mat4 WorldToLocal;
};


void Drawable::DrawGL4(SDFModel* Instance)
{

	if (Instance->Visibility == VisibilityStates::Imminent)
	{
		return;
	}

	if (!MeshUploaded)
	{
		IndexBuffer.Upload(Indices.data(), Indices.size() * sizeof(uint32_t));
		PositionBuffer.Upload(Positions.data(), Positions.size() * sizeof(glm::vec4));

		MeshUploaded = true;
	}
	{
		if (Instance->Colors.size() > 0)
		{
			Instance->ColorBuffer.Upload(Instance->Colors.data(), Instance->Colors.size() * sizeof(glm::vec4));

			IndexBuffer.Bind(GL_SHADER_STORAGE_BUFFER, 2);
			PositionBuffer.Bind(GL_SHADER_STORAGE_BUFFER, 3);
			Instance->ColorBuffer.Bind(GL_SHADER_STORAGE_BUFFER, 4);

			glDrawArrays(GL_TRIANGLES, 0, Indices.size());
		}
	}
}


void Drawable::DrawES2(
	const int PositionBinding,
	const int ColorBinding,
	SDFModel* Instance)
{
	if (Instance->Visibility == VisibilityStates::Imminent)
	{
		return;
	}

	if (!MeshUploaded)
	{
		IndexBuffer.Upload(GL_ELEMENT_ARRAY_BUFFER, GL_STATIC_DRAW, Indices.data(), Indices.size() * sizeof(uint32_t));
		PositionBuffer.Upload(GL_ARRAY_BUFFER, GL_STATIC_DRAW, Positions.data(), Positions.size() * sizeof(glm::vec4));

		MeshUploaded = true;
	}
	{
		if (Instance->Colors.size() > 0)
		{
			IndexBuffer.Bind(GL_ELEMENT_ARRAY_BUFFER);

			PositionBuffer.Bind(GL_ARRAY_BUFFER);
			glVertexAttribPointer(PositionBinding, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

			Instance->ColorBuffer.Upload(GL_ARRAY_BUFFER, GL_DYNAMIC_DRAW, Instance->Colors.data(), Instance->Colors.size() * sizeof(glm::vec4));
			Instance->ColorBuffer.Bind(GL_ARRAY_BUFFER);
			glVertexAttribPointer(ColorBinding, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

			glDrawElements(GL_TRIANGLES, Indices.size(), GL_UNSIGNED_INT, nullptr);
		}
	}
}


void SDFModel::UpdateColors(glm::vec3 NewCameraOrigin)
{
	AtomicCameraOrigin.store(NewCameraOrigin);

	for (size_t GroupIndex = 0; GroupIndex < ColoringGroups.size(); ++GroupIndex)
	{
		InstanceColoringGroupUnique& Batch = ColoringGroups[GroupIndex];

		std::vector<glm::vec4> NewColors;
		{
			Batch->ColorCS.lock();
			std::swap(Batch->Colors, NewColors);
			Batch->ColorCS.unlock();
		}

		if (NewColors.size() > 0)
		{
			std::vector<size_t>& Vertices = Batch->VertexGroup->Vertices;
			for (size_t RelativeIndex = 0; RelativeIndex < Batch->IndexRange; ++RelativeIndex)
			{
				const size_t VertexIndex = Vertices[Batch->IndexStart + RelativeIndex];
				Colors[VertexIndex] = NewColors[RelativeIndex];
			}
		}
	}
}


void SDFModel::DrawGL4(
	glm::vec3 CameraOrigin)
{
	if (Painter && Visibility != VisibilityStates::Invisible)
	{
		glm::mat4 LocalToWorldMatrix = LocalToWorld.ToMatrix();
		glm::mat4 WorldToLocalMatrix = glm::inverse(LocalToWorldMatrix);
		AtomicWorldToLocal.store(WorldToLocalMatrix);

		if (Visibility == VisibilityStates::Visible)
		{
			TransformUpload TransformData = {
				LocalToWorldMatrix,
				WorldToLocalMatrix
			};
			TransformBuffer.Upload((void*)&TransformData, sizeof(TransformUpload));
			TransformBuffer.Bind(GL_UNIFORM_BUFFER, 1);
		}

		UpdateColors(CameraOrigin);
		Painter->DrawGL4(this);
	}
}


void SDFModel::DrawES2(
	glm::vec3 CameraOrigin,
	const int LocalToWorldBinding,
	const int PositionBinding,
	const int ColorBinding)
{
	if (Painter && Visibility != VisibilityStates::Invisible)
	{
		glm::mat4 LocalToWorldMatrix = LocalToWorld.ToMatrix();
		glm::mat4 WorldToLocalMatrix = glm::inverse(LocalToWorldMatrix);
		AtomicWorldToLocal.store(WorldToLocalMatrix);

		if (Visibility == VisibilityStates::Visible)
		{
			glUniformMatrix4fv(LocalToWorldBinding, 1, false, (const GLfloat*)(&LocalToWorldMatrix));
		}

		UpdateColors(CameraOrigin);
		Painter->DrawES2(PositionBinding, ColorBinding, this);
	}
}
