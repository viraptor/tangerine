
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

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_SWIZZLE
#define GLM_FORCE_SINGLE_ONLY
#define GLM_ENABLE_EXPERIMENTAL

#ifdef _DEBUG
#define GLM_FORCE_PURE
#else
#define GLM_FORCE_INTRINSICS
#endif

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/mat4x4.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtx/extended_min_max.hpp>
#include <glm/gtx/quaternion.hpp>


// These are to patch over some differences between glsl and glm.
inline float min(float LHS, float RHS)
{
	return std::fminf(LHS, RHS);
}
inline float max(float LHS, float RHS)
{
	return std::fmaxf(LHS, RHS);
}
inline glm::vec2 max(glm::vec2 LHS, float RHS)
{
	return glm::max(LHS, glm::vec2(RHS));
}
inline glm::vec3 max(glm::vec3 LHS, float RHS)
{
	return glm::max(LHS, glm::vec3(RHS));
}
