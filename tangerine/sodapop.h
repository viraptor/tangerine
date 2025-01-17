
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

#include "sdf_rendering.h"


struct Drawable;
struct MeshingScratch;
struct SDFModel;
using DrawableShared = std::shared_ptr<Drawable>;
using SDFModelShared = std::shared_ptr<SDFModel>;


enum class MeshingAlgorithms
{
	Debug = 0,
	NaiveSurfaceNets = 1,
	SphereLatticeSearch = 2,
};


enum class VertexSequence : int
{
	// The vertex sequence is non-deterministic.
	Serendipity = 0,

	// Vertices are shuffled in memory after meshing.
	Shuffle,
};


enum class MaterialOverride
{
	Off,
	Normals,
	Invariant
};


namespace Sodapop
{
	void Populate(DrawableShared MeshPainter, float MeshingDensityPush);
	void Attach(SDFModelShared& Instance);
	void SetMaterialOverrideMode(MaterialOverride Mode);
}
