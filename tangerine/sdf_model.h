
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

#pragma once

#include "events.h"
#include "embedding.h"
#include "sdf_evaluator.h"
#include "sdf_rendering.h"
#include "transform.h"
#include "material.h"

#include "sodapop.h"
#include <atomic>
#include <mutex>

#include <string>
#include <map>


struct SDFModel;
using SDFModelShared = std::shared_ptr<SDFModel>;
using SDFModelWeakRef = std::weak_ptr<SDFModel>;


enum class VisibilityStates : int
{
	Invisible = 0,
	Imminent,
	Visible
};


void FlagSceneRepaint();
void PostPendingRepaintRequest();


struct Drawable
{
	std::string Name = "unknown";

	SDFNodeShared Evaluator = nullptr;

	Buffer IndexBuffer;
	Buffer PositionBuffer;

	std::vector<uint32_t> Indices;
	std::vector<glm::vec4> Positions;
	std::vector<glm::vec4> Normals;
	std::vector<glm::vec4> Colors;

	bool MeshAvailable = false;
	bool MeshUploaded = false;

	VertexSequence VertexOrderHint = VertexSequence::Shuffle;
	MeshingAlgorithms MeshingAlgorithm = MeshingAlgorithms::NaiveSurfaceNets;

	// These are populated during the meshing process, but may be safely used after the mesh is ready.
	SDFOctreeShared EvaluatorOctree = nullptr;
	std::vector<MaterialVertexGroup> MaterialSlots;
	std::mutex MaterialSlotsCS;
	std::map<MaterialShared, size_t> SlotLookup;

	uint64_t MeshingFrameStart = 0;
	uint64_t MeshingFrameComplete = 0;
	uint64_t MeshingFrameLatency = 0;

	Drawable(const std::string& InName, SDFNodeShared& InEvaluator);

	void DrawGL4(SDFModel* Instance);

	void DrawES2(
		const int PositionBinding,
		const int ColorBinding,
		SDFModel* Instance);

	~Drawable();
};

using DrawableShared = std::shared_ptr<Drawable>;
using DrawableWeakRef = std::weak_ptr<Drawable>;


struct InstanceColoringGroup
{
	MaterialVertexGroup* VertexGroup;
	size_t IndexStart; // Offset within vertex group
	size_t IndexRange; // Number of indices to process

	std::mutex ColorCS;
	std::vector<glm::vec4> Colors;

	uint64_t LastRepaint = 0;

	InstanceColoringGroup(MaterialVertexGroup* InVertexGroup, size_t InIndexStart, size_t InIndexRange)
		: VertexGroup(InVertexGroup)
		, IndexStart(InIndexStart)
		, IndexRange(InIndexRange)
	{
	}

	bool StartRepaint();
};

using InstanceColoringGroupUnique = std::unique_ptr<InstanceColoringGroup>;


struct SDFModel
{
	SDFNodeShared Evaluator = nullptr;
	DrawableShared Painter = nullptr;

	std::atomic<VisibilityStates> Visibility = VisibilityStates::Visible;
	Transform LocalToWorld;
	Buffer TransformBuffer;
	std::atomic<glm::mat4> AtomicWorldToLocal;
	std::atomic<glm::vec3> AtomicCameraOrigin = glm::vec3(0.0, 0.0, 0.0);

	int MouseListenFlags = 0;

	std::string Name = "";

	std::vector<glm::vec4> Colors;
	Buffer ColorBuffer;

	std::vector<InstanceColoringGroupUnique> ColoringGroups;

	void UpdateColors(glm::vec3 CameraOrigin);

	void DrawGL4(glm::vec3 CameraOrigin);

	void DrawES2(
		glm::vec3 CameraOrigin,
		const int LocalToWorldBinding,
		const int PositionBinding,
		const int ColorBinding);

	RayHit RayMarch(glm::vec3 RayStart, glm::vec3 RayDir, int MaxIterations = 1000, float Epsilon = 0.001);

	static SDFModelShared Create(std::shared_ptr<class PaintingSet>& Locus, SDFNodeShared& InEvaluator, const std::string& InName,
		const float VoxelSize = 0.25, const float MeshingDensityPush = 0.0, const VertexSequence VertexOrderHint = VertexSequence::Shuffle);

	SDFModel(SDFModel&& Other) = delete;
	virtual ~SDFModel();

	virtual void OnMouseEvent(MouseEvent& Event, bool Picked) {};

protected:
	static void RegisterNewModel(std::shared_ptr<class PaintingSet>& Locus, SDFModelShared& NewModel);
	SDFModel(SDFNodeShared& InEvaluator, const std::string& InName, const float VoxelSize, const float MeshingDensityPush, const VertexSequence VertexOrderHint);
};


std::vector<std::pair<size_t, DrawableWeakRef>>& GetDrawableCache();
void MeshReady(DrawableShared Painter);

bool DeliverMouseMove(glm::vec3 Origin, glm::vec3 RayDir, int MouseX, int MouseY);
bool DeliverMouseButton(MouseEvent Event);
bool DeliverMouseScroll(glm::vec3 Origin, glm::vec3 RayDir, int ScrollX, int ScrollY);

void ClearTreeEvaluator();
void SetTreeEvaluator(SDFNodeShared& InTreeEvaluator);
