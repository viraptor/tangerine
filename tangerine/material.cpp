
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

#include "material.h"


bool MaterialInterface::operator==(MaterialInterface& Other)
{
	return this == &Other;
}


glm::vec4 MaterialSolidColor::Eval(glm::vec3 Point, glm::vec3 N, glm::vec3 V)
{
	return glm::vec4(SampleColor(BaseColor), 1.0f);
}


glm::vec4 MaterialPBRBR::Eval(glm::vec3 Point, glm::vec3 N, glm::vec3 V)
{
	// Palecek 2022, "PBR Based Rendering"
	float D = glm::pow(glm::max(glm::dot(N, glm::normalize(N * 0.75f + V)), 0.0f), 2.0f);
	float F = 1.0f - glm::max(glm::dot(N, V), 0.0f);
	float BSDF = D + F * 0.25f;
	return glm::vec4(SampleColor(BaseColor) * BSDF, 1.0f);
}


glm::vec4 MaterialDebugNormals::StaticEval(glm::vec3 Normal)
{
	return glm::vec4(Normal * glm::vec3(0.5) + glm::vec3(0.5), 1.0);
}


glm::vec4 MaterialDebugNormals::Eval(glm::vec3 Point, glm::vec3 Normal, glm::vec3 View)
{
	return StaticEval(Normal);
}


glm::vec4 MaterialDebugGradient::Eval(glm::vec3 Point, glm::vec3 Normal, glm::vec3 View)
{
	float Alpha = glm::fract(SDF->Eval(Point) / Interval);
	return glm::vec4(Ramp.Eval(ColorSpace::sRGB, Alpha), 1.0);
}


glm::vec4 MaterialGradientLight::Eval(glm::vec3 Point, glm::vec3 Normal, glm::vec3 View, glm::vec3 Light)
{
	float Alpha = glm::clamp(glm::dot(Normal, -Light), 0.0f, 1.0f);
	//float Alpha = glm::clamp(-glm::dot(Normal, Light), -1.0f, 1.0f) * 0.5 + 0.5;
	return glm::vec4(Ramp.Eval(ColorSpace::sRGB, Alpha), 1.0);
}
