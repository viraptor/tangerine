
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

#include <glad/gl.h>

#ifdef MINIMAL_DLL
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>
#include <SDL_video.h>
#include <SDL_opengl.h>
#include <SDL_clipboard.h>

#ifdef ENABLE_RMLUI
#include <RmlUi/Core.h>
#include <RmlUi_Platform_SDL.h>
#include <RmlUi_Renderer_GL3.h>
#include <RmlUi/Debugger.h>

SystemInterface_SDL* RmlUiSystemInterface = nullptr;
RenderInterface_GL3* RmlUiRenderInterface = nullptr;
Rml::Context* RmlUiContext = nullptr;
Rml::ElementDocument* RmlUiDocument = nullptr;
#endif

#include <imgui.h>
#include <imgui_impl_sdl.h>
#include <imgui_impl_opengl3.h>
#include <ImFileDialog.h>

#include "license_page.h"

#include "gl_init.h"
#include "scheduler.h"

#include "events.h"
#include "sdf_evaluator.h"
#include "sdf_rendering.h"
#include "sdf_model.h"
#include "painting_set.h"
#include "controller.h"
#include "units.h"
#include "export.h"
#include "magica.h"
#include "extern.h"

#include "lua_env.h"
#include "racket_env.h"
#include "tangerine.h"

#include "psmove_loader.h"

#if !_WIN64
#include "linux.h"
#endif

#include <iostream>
#include <iterator>
#include <cstring>
#include <map>
#include <vector>
#include <chrono>
#include <regex>
#include <filesystem>
#include <fstream>
#include <bit>

#include <fmt/format.h>

#include "profiling.h"

#include "errors.h"
#include "gl_boilerplate.h"
#include "gl_debug.h"
#include "installation.h"


// TODO: These were originally defined as a cross-platform compatibility hack for code
// in this file that was using min/max macros from a Windows header.  The header is no
// longer in use, but the code remains.
#define min(a, b) (a < b ? a : b)
#define max(a, b) (a > b ? a : b)


static std::atomic<uint64_t> AtomicFrameNumber = 0;
uint64_t GetFrameNumber()
{
	return AtomicFrameNumber.load();
}


bool HeadlessMode;
bool ShowDebugInterface = true;
bool OverrideShowDebugInterface = false;
bool ShowLicenses = false;


static ScriptEnvironment* MainEnvironment = nullptr;

ScriptEnvironment* GetMainEnvironment()
{
	return MainEnvironment;
}


SDFNodeShared TreeEvaluator = nullptr;


TangerinePaths Installed;
std::filesystem::path LastOpenDir;


extern SDL_Window* Window;
extern SDL_GLContext Context;


AABB ModelBounds = { glm::vec3(0.0), glm::vec3(0.0) };
void SetTreeEvaluator(SDFNodeShared& InTreeEvaluator)
{
	TreeEvaluator = InTreeEvaluator;
	ModelBounds = TreeEvaluator->Bounds();
}


void ClearTreeEvaluator()
{
	TreeEvaluator.reset();
}

GLsizei MultiSampleCount = 8;

ShaderProgram NoiseShader;
ShaderProgram BgShader;
ShaderProgram SodapopShader;

Buffer ViewInfo("ViewInfo Buffer");
Buffer DepthTimeBuffer("Subtree Heatmap Buffer");

GLuint ColorPass;
GLuint ForwardPass;
GLuint FinalPass = 0;

GLuint DepthBuffer;
GLuint ColorBuffer;
GLuint FinalBuffer;

TimingQuery DepthTimeQuery;
TimingQuery GridBgTimeQuery;
TimingQuery UiTimeQuery;

const glm::vec3 DefaultBackgroundColor = glm::vec3(.6);
glm::vec3 BackgroundColor = DefaultBackgroundColor;


void AllocateRenderTargets(int ScreenWidth, int ScreenHeight)
{
	static bool Initialized = false;
	if (Initialized)
	{
		glDeleteFramebuffers(1, &ColorPass);
		glDeleteFramebuffers(1, &ForwardPass);
		glDeleteTextures(1, &DepthBuffer);
		glDeleteTextures(1, &ColorBuffer);
		if (HeadlessMode)
		{
			glDeleteFramebuffers(1, &FinalPass);
			glDeleteTextures(1, &FinalBuffer);
		}
	}
	else
	{
		Initialized = true;
	}

	if (GraphicsBackend == GraphicsAPI::OpenGL4_2)
	{
		// Color buffer
		if (MultiSampleCount > 1)
		{
			glCreateTextures(GL_TEXTURE_2D_MULTISAMPLE, 1, &ColorBuffer);
			glTextureStorage2DMultisample(ColorBuffer, MultiSampleCount, GL_RGB8, ScreenWidth, ScreenHeight, GL_FALSE);
			glObjectLabel(GL_TEXTURE, ColorBuffer, -1, "MSAA Color Buffer");
		}
		else
		{
			glCreateTextures(GL_TEXTURE_2D, 1, &ColorBuffer);
			glTextureStorage2D(ColorBuffer, 1, GL_RGB8, ScreenWidth, ScreenHeight);
			glObjectLabel(GL_TEXTURE, ColorBuffer, -1, "Color Buffer");
		}

		// Depth buffer
		if (MultiSampleCount > 1)
		{
			glCreateTextures(GL_TEXTURE_2D_MULTISAMPLE, 1, &DepthBuffer);
			glTextureStorage2DMultisample(DepthBuffer, MultiSampleCount, GL_DEPTH_COMPONENT32F, ScreenWidth, ScreenHeight, GL_FALSE);
			glObjectLabel(GL_TEXTURE, DepthBuffer, -1, "MSAA Depth Buffer");
		}
		else
		{
			glCreateTextures(GL_TEXTURE_2D, 1, &DepthBuffer);
			glTextureStorage2D(DepthBuffer, 1, GL_DEPTH_COMPONENT32F, ScreenWidth, ScreenHeight);
			glObjectLabel(GL_TEXTURE, DepthBuffer, -1, "Depth Buffer");
		}

		// Depthless color pass for background drawing
		{
			glCreateFramebuffers(1, &ColorPass);
			glObjectLabel(GL_FRAMEBUFFER, ColorPass, -1, "Color Pass");
			glNamedFramebufferTexture(ColorPass, GL_COLOR_ATTACHMENT0, ColorBuffer, 0);
			GLenum ColorAttachments[1] = { GL_COLOR_ATTACHMENT0 };
			glNamedFramebufferDrawBuffers(ColorPass, 1, ColorAttachments);
		}

		// Forward pass for rendering objects
		{
			glCreateFramebuffers(1, &ForwardPass);
			glObjectLabel(GL_FRAMEBUFFER, ForwardPass, -1, "Forward Rendering Pass");
			glNamedFramebufferTexture(ForwardPass, GL_DEPTH_ATTACHMENT, DepthBuffer, 0);
			glNamedFramebufferTexture(ForwardPass, GL_COLOR_ATTACHMENT0, ColorBuffer, 0);
			GLenum ColorAttachments[1] = { GL_COLOR_ATTACHMENT0 };
			glNamedFramebufferDrawBuffers(ForwardPass, 1, ColorAttachments);
		}
	}

	// Final pass
	if (HeadlessMode)
	{
		glCreateTextures(GL_TEXTURE_2D, 1, &FinalBuffer);
		glTextureStorage2D(FinalBuffer, 1, GL_RGB8, int(ScreenWidth), int(ScreenHeight));
		glTextureParameteri(FinalBuffer, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTextureParameteri(FinalBuffer, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTextureParameteri(FinalBuffer, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTextureParameteri(FinalBuffer, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glObjectLabel(GL_TEXTURE, FinalBuffer, -1, "FinalBuffer");

		glCreateFramebuffers(1, &FinalPass);
		glObjectLabel(GL_FRAMEBUFFER, FinalPass, -1, "FinalPass");
		glNamedFramebufferTexture(FinalPass, GL_COLOR_ATTACHMENT0, FinalBuffer, 0);
		GLenum ColorAttachments[1] = { GL_COLOR_ATTACHMENT0 };
		glNamedFramebufferDrawBuffers(FinalPass, 1, ColorAttachments);
	}
}


void DumpFrameBuffer(int ScreenWidth, int ScreenHeight, std::vector<unsigned char>& PixelData)
{
	const size_t Channels = 3;
	PixelData.resize(size_t(ScreenWidth) * size_t(ScreenHeight) * Channels);
	glNamedFramebufferReadBuffer(FinalPass, GL_COLOR_ATTACHMENT0);
	glReadPixels(0, 0, GLsizei(ScreenWidth), GLsizei(ScreenHeight), GL_RGB, GL_UNSIGNED_BYTE, PixelData.data());
}


void EncodeBase64(std::vector<unsigned char>& Bytes, std::vector<char>& Encoded)
{
	const std::string Base64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	// Every three input bytes produces 4 output characters.  To keep things simple,
	// if we are short a few, the input sequence will be 0-padded to fill out the remainder.

	const size_t Words = (Bytes.size() + 2) / 3; // Divide by 3, rounding up.
	const size_t BytesPerWord = 3;
	const size_t GlyphsPerWord = 4;
	const size_t BitsPerGlyph = 6;

	Encoded.reserve(Words * GlyphsPerWord);
	for (int w = 0; w < Words; ++w)
	{
		uint32_t Chunk = 0;
		for (int b = 0; b < BytesPerWord; ++b)
		{
			Chunk <<= 8;
			const int i = BytesPerWord * w + b;
			if (i < Bytes.size())
			{
				Chunk |= Bytes[i];
			}
		}

		int Shift = GlyphsPerWord * BitsPerGlyph;
		for (int g = 0; g < GlyphsPerWord; ++g)
		{
			const int Inverse = (GlyphsPerWord - 1) - g;
			const size_t Glyph = (Chunk >> (Inverse * BitsPerGlyph)) & 63;
			if (Glyph < Base64.size())
			{
				Encoded.push_back(Base64[Glyph]);
			}
		}
	}
}


void SetPipelineDefaults()
{
	if (GraphicsBackend == GraphicsAPI::OpenGL4_2)
	{
		// For drawing without a VBO bound.
		static GLuint NullVAO;
		static bool CreatedNullVAO = false;
		if (!CreatedNullVAO)
		{
			CreatedNullVAO = true;
			glGenVertexArrays(1, &NullVAO);
		}
		glBindVertexArray(NullVAO);

		// These don't appear to be available in ES2 :(
		glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
	}
	{
		glDepthRangef(1.0, 0.0);
		glDisable(GL_DITHER);
		glEnable(GL_CULL_FACE);
		glEnable(GL_DEPTH_TEST);
	}
}


// Renderer setup.
StatusCode SetupRenderer()
{
	SetPipelineDefaults();

	glClearColor(0.0, 0.0, 0.0, 1.0);
	glClearDepthf(0.0);

	if (GraphicsBackend == GraphicsAPI::OpenGL4_2)
	{
		RETURN_ON_FAIL(BgShader.Setup(
			{ {GL_VERTEX_SHADER, ShaderSource("splat.vs.glsl", true)},
			{GL_FRAGMENT_SHADER, ShaderSource("bg.fs.glsl", true)} },
			"Background Shader"));

		RETURN_ON_FAIL(NoiseShader.Setup(
			{ {GL_VERTEX_SHADER, ShaderSource("splat.vs.glsl", true)},
			{GL_FRAGMENT_SHADER, ShaderSource("noise.fs.glsl", true)} },
			"Noise Shader"));
	}
	else if (GraphicsBackend == GraphicsAPI::OpenGLES2)
	{
		RETURN_ON_FAIL(NoiseShader.Setup(
			{ {GL_VERTEX_SHADER, ShaderSource("splat.vs.glsl", true)},
			{GL_FRAGMENT_SHADER, ShaderSource("nosignal.fs.glsl", true)} },
			"No Signal Shader"));
	}

	RETURN_ON_FAIL(SodapopShader.Setup(
		{ {GL_VERTEX_SHADER, ShaderSource("sodapop.vs.glsl", true)},
		{GL_FRAGMENT_SHADER, ShaderSource("sodapop.fs.glsl", true)} },
		"Sodapop Shader"));

	DepthTimeQuery.Create();
	GridBgTimeQuery.Create();
	UiTimeQuery.Create();

	return StatusCode::PASS;
}


bool UsePerspective = true;
float OrthoScale = 64.0;
float CameraFov = 45.0;
float CameraNear = 0.1;
float CameraFar = 1000.0;
glm::mat4 GetViewToClip(int ViewportWidth, int ViewportHeight)
{
	const float AspectRatio = float(ViewportWidth) / float(ViewportHeight);
	if (UsePerspective)
	{
		return glm::infinitePerspective(glm::radians(CameraFov), AspectRatio, CameraNear);
	}
	else
	{
		float Scale = (1.0 / OrthoScale) * .5;
		float Horizontal = ViewportWidth * Scale;
		float Vertical = ViewportHeight * Scale;
		return glm::ortho(-Horizontal, Horizontal, -Vertical, Vertical, CameraNear, CameraFar);
	}
}


bool FlushPendingFileDialogTextureDeletes = false;
std::vector<GLuint> PendingFileDialogTextureDeletes;

void ProcessPendingFileDialogTextureDeletes()
{
	size_t Count = PendingFileDialogTextureDeletes.size();
	if (Count > 0)
	{
		glDeleteTextures(Count, PendingFileDialogTextureDeletes.data());
		PendingFileDialogTextureDeletes.clear();
		FlushPendingFileDialogTextureDeletes = false;
	}
}


int BackgroundMode = 1;
int UserRequestedBackground = -1;
int ProgramRequestedBackground = -1;

int GetBackgroundMode()
{
	if (GraphicsBackend == GraphicsAPI::OpenGLES2)
	{
		// Only the solid color mode is supported right now.
		return 0;
	}
	else
	{
		int RequestedMode = BackgroundMode;

		if (UserRequestedBackground > -1)
		{
			RequestedMode = UserRequestedBackground;
		}
		else if (ProgramRequestedBackground > -1)
		{
			RequestedMode = ProgramRequestedBackground;
		}

		return RequestedMode;
	}
}


int MouseMotionX = 0;
int MouseMotionY = 0;
int MouseMotionZ = 0;

bool FixedCamera = false;
glm::vec3 FixedOrigin = glm::vec3(0, -1, 0);
glm::vec3 FixedFocus = glm::vec3(0, 0, 0);
glm::vec3 FixedUp = glm::vec3(0, 0, 1);

bool ResetCamera = true;
bool RealtimeMode = false;
bool ShowStatsOverlay = false;
float PresentFrequency = 0.0;
float PresentDeltaMs = 0.0;
double LastInnerFrameDeltaMs = 0.0;
glm::vec3 CameraFocus = glm::vec3(0.0, 0.0, 0.0);

namespace SchedulerStats
{
	size_t Inbox;
	size_t Outbox;
	size_t ParallelQueue;
	size_t ContinuousQueue;
	size_t DeleteQueue;
}


// Total CPU time spent in per-model drawing paths
double TotalDrawTimeMS = 0.0;

// Total CPU time spent stalled on present
double PresentTimeMs = 0.0;


void RenderEmptyFrameGL4(int ScreenWidth, int ScreenHeight, ViewInfoUpload& UploadedView);


void RenderEmptyFrameES2(int ScreenWidth, int ScreenHeight, ViewInfoUpload& UploadedView);


void RenderFrame(int ScreenWidth, int ScreenHeight, ViewInfoUpload& UploadedView)
{
	BeginEvent("RenderFrame");
	ProfilingTimePoint FrameStartTimePoint = ProfilingClock::now();

	double CurrentTime;
	{
		static ProfilingTimePoint StartTimePoint = FrameStartTimePoint;
		static ProfilingTimePoint LastTimePoint = StartTimePoint;
		{
			std::chrono::duration<double, std::milli> FrameDelta = FrameStartTimePoint - LastTimePoint;
			PresentDeltaMs = float(FrameDelta.count());
		}
		{
			std::chrono::duration<double, std::milli> EpochDelta = FrameStartTimePoint - StartTimePoint;
			CurrentTime = float(EpochDelta.count());
		}
		LastTimePoint = FrameStartTimePoint;
		PresentFrequency = float(1000.0 / PresentDeltaMs);
	}

	static int FrameNumber = 0;
	++FrameNumber;

	TotalDrawTimeMS = 0.0;

	static int Width = 0;
	static int Height = 0;
	{
		if (ScreenWidth != Width || ScreenHeight != Height)
		{
			Width = ScreenWidth;
			Height = ScreenHeight;
			glViewport(0, 0, Width, Height);
			if (GraphicsBackend == GraphicsAPI::OpenGL4_2)
			{
				AllocateRenderTargets(Width, Height);
			}
		}
	}

	if (FixedCamera)
	{
		const glm::mat4 WorldToView = glm::lookAt(FixedOrigin, FixedFocus, FixedUp);
		const glm::mat4 ViewToWorld = glm::inverse(WorldToView);

		const glm::mat4 ViewToClip = GetViewToClip(Width, Height);
		const glm::mat4 ClipToView = inverse(ViewToClip);

		UploadedView = {
			WorldToView,
			ViewToWorld,
			ViewToClip,
			ClipToView,
			glm::vec4(FixedOrigin, 1.0f),
			glm::vec4(Width, Height, 1.0f / Width, 1.0f / Height),
			glm::vec4(ModelBounds.Min, 1.0),
			glm::vec4(ModelBounds.Max, 1.0),
			float(CurrentTime),
			UsePerspective,
		};
	}
	else
	{
		static float RotateX;
		static float RotateZ;
		static float Zoom;
		if (ResetCamera)
		{
			ResetCamera = false;
			RotateX = 0.0;
			RotateZ = 0.0;
			Zoom = 14.0;
			CameraFocus = (ModelBounds.Max - ModelBounds.Min) * glm::vec3(0.5) + ModelBounds.Min;
		}

		RotateX = fmodf(RotateX - MouseMotionY, 360.0);
		RotateZ = fmodf(RotateZ - MouseMotionX, 360.0);
		Zoom = fmaxf(0.0, Zoom - MouseMotionZ);
		const float ToRadians = float(M_PI / 180.0);

		glm::mat4 Orientation = glm::identity<glm::mat4>();
		Orientation = glm::rotate(Orientation, RotateZ * ToRadians, glm::vec3(0.0, 0.0, 1.0));
		Orientation = glm::rotate(Orientation, RotateX * ToRadians, glm::vec3(1.0, 0.0, 0.0));

		glm::vec4 Fnord = Orientation * glm::vec4(0.0, -Zoom, 0.0, 1.0);
		glm::vec3 CameraOrigin = glm::vec3(Fnord.x, Fnord.y, Fnord.z) / Fnord.w;

		Fnord = Orientation * glm::vec4(0.0, 0.0, 1.0, 1.0);
		glm::vec3 UpDir = glm::vec3(Fnord.x, Fnord.y, Fnord.z) / Fnord.w;

		const glm::mat4 WorldToView = glm::lookAt(CameraFocus + CameraOrigin, CameraFocus, UpDir);
		const glm::mat4 ViewToWorld = glm::inverse(WorldToView);

		{
			glm::vec4 CameraLocal = ViewToWorld * glm::vec4(0.0, 0.0, 0.0, 1.0);
			CameraLocal /= CameraLocal.w;
			CameraOrigin = glm::vec3(CameraLocal.x, CameraLocal.y, CameraLocal.z);
		}

		const glm::mat4 ViewToClip = GetViewToClip(Width, Height);
		const glm::mat4 ClipToView = inverse(ViewToClip);

		UploadedView = {
			WorldToView,
			ViewToWorld,
			ViewToClip,
			ClipToView,
			glm::vec4(CameraOrigin, 1.0f),
			glm::vec4(Width, Height, 1.0f / Width, 1.0f / Height),
			glm::vec4(ModelBounds.Min, 1.0),
			glm::vec4(ModelBounds.Max, 1.0),
			float(CurrentTime),
			UsePerspective,
		};
	}

	static glm::mat4 LastWorldToView = glm::identity<glm::mat4>();
	if (UploadedView.WorldToView != LastWorldToView)
	{
		LastWorldToView = UploadedView.WorldToView;
		FlagSceneRepaint();
	}

	bool AnyReady = false;
	ScriptEnvironment* Env = GetMainEnvironment();
	for (PaintingSetShared Group : Env->PaintingSets)
	{
		if (Group->CanRender())
		{
			AnyReady = true;
			break;
		}
	}

	if (AnyReady)
	{
		for (PaintingSetShared Group : Env->PaintingSets)
		{
			Group->RenderFrame(ScreenWidth, ScreenHeight, UploadedView);
		}
	}
	else
	{
		if (GraphicsBackend == GraphicsAPI::OpenGL4_2)
		{
			RenderEmptyFrameGL4(ScreenWidth, ScreenHeight, UploadedView);
		}
		else if (GraphicsBackend == GraphicsAPI::OpenGLES2)
		{
			RenderEmptyFrameES2(ScreenWidth, ScreenHeight, UploadedView);
		}
	}

	// Needs to occur after rendering to prevent stale coloring groups.
	PostPendingRepaintRequest();

	{
		std::chrono::duration<double, std::milli> InnerFrameDelta = ProfilingClock::now() - FrameStartTimePoint;
		LastInnerFrameDeltaMs = float(InnerFrameDelta.count());
	}

	EndEvent();
}


void RenderEmptyFrameGL4(int ScreenWidth, int ScreenHeight, ViewInfoUpload& UploadedView)
{
	glDisable(GL_FRAMEBUFFER_SRGB);

	ViewInfo.Upload((void*)&UploadedView, sizeof(UploadedView));
	ViewInfo.Bind(GL_UNIFORM_BUFFER, 0);

	{
		glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, -1, "Dead Channel");
		glDepthMask(GL_FALSE);
		glDisable(GL_DEPTH_TEST);
		glBindFramebuffer(GL_FRAMEBUFFER, ForwardPass);
		glBindTextureUnit(1, ColorBuffer);
		NoiseShader.Activate();
		glDrawArrays(GL_TRIANGLES, 0, 3);
		glPopDebugGroup();
	}
}


void RenderEmptyFrameES2(int ScreenWidth, int ScreenHeight, ViewInfoUpload& UploadedView)
{
	glDisable(GL_FRAMEBUFFER_SRGB);
	glBindFramebuffer(GL_FRAMEBUFFER, FinalPass);

	{
		glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, -1, "Dead Channel");
		glDepthMask(GL_FALSE);
		glDisable(GL_DEPTH_TEST);
		NoiseShader.Activate();

		static glm::vec2 SplatVerts[3] = \
		{
			glm::vec2(-1.0, -1.0),
			glm::vec2(3.0, -1.0),
			glm::vec2(-1.0, 3.0)
		};

		GLint ClipAttrib = glGetAttribLocation(NoiseShader.ProgramID, "Clip");
		glVertexAttribPointer(ClipAttrib, 2, GL_FLOAT, GL_FALSE, 0, (void*)(&SplatVerts));
		glEnableVertexAttribArray(ClipAttrib);

		glDrawArrays(GL_TRIANGLES, 0, 3);
		glPopDebugGroup();
	}
}


void ResolveFrame(int PixelWidth, int PixelHeight)
{
	if (GraphicsBackend == GraphicsAPI::OpenGL4_2)
	{
		glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, -1, "Resolve Output");
		glBindFramebuffer(GL_READ_FRAMEBUFFER, ColorPass);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, FinalPass);
		glBlitFramebuffer(0, 0, PixelWidth, PixelHeight, 0, 0, PixelWidth, PixelHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		glPopDebugGroup();
	}
}


void SetWindowTitle(std::string Title)
{
	if (Window != nullptr)
	{
		SDL_SetWindowTitle(Window, Title.c_str());
	}
}


void ShowDebugMenu()
{
	ShowDebugInterface = true;
}


void HideDebugMenu()
{
	ShowDebugInterface = false;
}


void SetClearColor(glm::vec3& Color)
{
	ProgramRequestedBackground = 0;
	BackgroundColor = Color;
}


void SetFixedCamera(glm::vec3& Origin, glm::vec3& Focus, glm::vec3& Up)
{
	FixedCamera = true;
	FixedOrigin = Origin;
	FixedFocus = Focus;
	FixedUp = Up;
}


void ToggleFullScreen(SDL_Window* Window)
{
	static bool FullScreen = false;
	FullScreen = !FullScreen;
	SDL_SetWindowFullscreen(Window, FullScreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}


std::vector<std::string> ScriptErrors;
void PostScriptError(std::string ErrorMessage)
{
	std::cout << ErrorMessage << "\n";
	ScriptErrors.push_back(ErrorMessage);
}


double ModelProcessingStallMs = 0.0;
void LoadModelCommon(std::function<void()> LoadingCallback)
{
	BeginEvent("Load Model");

	ClearTreeEvaluator();
	Scheduler::DropEverything();

	FixedCamera = false;

	if (UserRequestedBackground > -1)
	{
		BackgroundMode = UserRequestedBackground;
	}
	UserRequestedBackground = -1;
	ProgramRequestedBackground = -1;
	BackgroundColor = DefaultBackgroundColor;

	ProfilingTimePoint StartTimePoint = ProfilingClock::now();

	LoadingCallback();

	if (MainEnvironment)
	{
		EnvInitialControllerConnections(*MainEnvironment);
	}

	ProfilingTimePoint EndTimePoint = ProfilingClock::now();
	std::chrono::duration<double, std::milli> Delta = EndTimePoint - StartTimePoint;
	ModelProcessingStallMs = Delta.count();
	EndEvent();
}


void CreateScriptEnvironment(const Language Runtime)
{
	switch (Runtime)
	{
	case Language::Lua:
#if EMBED_LUA
		delete MainEnvironment;
		MainEnvironment = new LuaEnvironment();
#else
		ScriptErrors.push_back(std::string("The Lua language runtime is not available in this build :(\n"));
#endif
		break;

	case Language::Racket:
#if EMBED_RACKET
		delete MainEnvironment;
		MainEnvironment = new RacketEnvironment();
#else
		ScriptErrors.push_back(std::string("The Racket language runtime is not available in this build :(\n"));
#endif
		break;

	default:
		ScriptErrors.push_back(std::string("Unknown source language.\n"));
	}
}


void LoadModel(std::string Path, Language Runtime)
{
	static std::string LastPath = "";
	if (Path.size() == 0)
	{
		// Reload
		Path = LastPath;
		Runtime = MainEnvironment->GetLanguage();
	}
	else
	{
		ResetCamera = true;
	}
	if (Path.size() > 0)
	{
		ShowDebugInterface = true;
		{
			std::string FileName = std::filesystem::path(Path).filename().string();
			SetWindowTitle(fmt::format("{} - Tangerine", FileName));
		}
		ExportGrid::ResetScale();
		CreateScriptEnvironment(Runtime);
		LastPath = Path;
		MainEnvironment->LoadFromPath(Path);
	}
}


void ReloadModel()
{
	LoadModel("", Language::Unknown);
}


void LoadFromPipe(Language Runtime)
{
	std::istreambuf_iterator<char> begin(std::cin), end;
	std::string Source(begin, end);

	if (Source.size() > 0)
	{
		std::cout << "Evaluating data from stdin.\n";
		ExportGrid::ResetScale();
		CreateScriptEnvironment(Runtime);
		MainEnvironment->LoadFromString(Source);
		std::cout << "Done!\n";
	}
	else
	{
		std::cout << "No data provided.\n";
	}
}


Language LanguageForPath(std::string Path)
{
	const std::regex LuaFile(".*?\\.(lua)$", std::regex::icase);
	const std::regex RacketFile(".*?\\.(rkt)$", std::regex::icase);

	if (std::regex_match(Path, LuaFile))
	{
		return Language::Lua;
	}
	else if (std::regex_match(Path, RacketFile))
	{
		return Language::Racket;
	}
	else
	{
		return Language::Unknown;
	}
}


ExportFormat ExportFormatForPath(std::string Path)
{
	const std::regex PlyFile(".*?\\.(ply)$", std::regex::icase);
	const std::regex StlFile(".*?\\.(stl)$", std::regex::icase);
	const std::regex VoxFile(".*?\\.(vox)$", std::regex::icase);

	if (std::regex_match(Path, PlyFile))
	{
		return ExportFormat::PLY;
	}
	else if (std::regex_match(Path, StlFile))
	{
		return ExportFormat::STL;
	}
	else if (std::regex_match(Path, VoxFile))
	{
		return ExportFormat::VOX;
	}
	else
	{
		Assert(false);
		return ExportFormat::Unknown;
	}
}


void OpenModel()
{
	std::string Filter = "";
	{
		std::string Separator = "";

#if EMBED_MULTI
		// TODO: This will need to be revised if--Madoka help me--I decide to embed another optional language runtime.
		Filter = fmt::format("{}{}Tangerines (*.lua; *.rkt){{.lua,.rkt}}", Filter, Separator);
		Separator = ",";
#endif

#if EMBED_LUA
		Filter = fmt::format("{}{}Lua Sources (*.lua){{.lua}}", Filter, Separator);
		Separator = ",";
#endif

#if EMBED_RACKET
		Filter = fmt::format("{}{}Racket Sources (*.rkt){{.rkt}}", Filter, Separator);
		Separator = ",";
#endif

		Filter = fmt::format("{}{}.*", Filter, Separator);
	}

	FlushPendingFileDialogTextureDeletes = false;
	ifd::FileDialog::Instance().Open("OpenModelDialog", "Open a model", Filter, false, LastOpenDir.string());
}


void WorldSpaceRay(const ViewInfoUpload& View, int ScreenX, int ScreenY, int ScreenWidth, int ScreenHeight, glm::vec3& Origin, glm::vec3& Direction)
{
	glm::vec4 ViewPosition;

	if (UsePerspective)
	{
		glm::vec4 ClipPosition(
			glm::clamp(float(ScreenX) / float(ScreenWidth), 0.0f, 1.0f) * 2.0 - 1.0,
			glm::clamp(float(ScreenHeight - ScreenY) / float(ScreenHeight), 0.0f, 1.0f) * 2.0 - 1.0,
			-1.0f,
			1.0f);
		ViewPosition = View.ClipToView * ClipPosition;

		Origin = View.CameraOrigin;
	}
	else
	{
		glm::vec4 ClipPosition(
			glm::clamp(float(ScreenX) / float(ScreenWidth), 0.0f, 1.0f) * 2.0 - 1.0,
			glm::clamp(float(ScreenHeight - ScreenY) / float(ScreenHeight), 0.0f, 1.0f) * 2.0 - 1.0,
			1.0f,
			1.0f);
		ViewPosition = View.ClipToView * ClipPosition;

		glm::vec4 ViewOrigin = glm::vec4(ViewPosition.xy(), 0.0, ViewPosition.w);
		glm::vec4 WorldOrigin = View.ViewToWorld * ViewOrigin;
		Origin = glm::vec3(WorldOrigin.xyz()) / WorldOrigin.w;
	}

	{
		glm::vec4 WorldPosition = View.ViewToWorld * ViewPosition;
		WorldPosition /= WorldPosition.w;
		glm::vec3 Ray = glm::vec3(WorldPosition.xyz()) - glm::vec3(Origin);
		Direction = glm::normalize(Ray);
	}
}

// These are GPU times
double DepthElapsedTimeMs = 0.0;
double GridBgElapsedTimeMs = 0.0;
double UiElapsedTimeMs = 0.0;

void RenderUI(SDL_Window* Window, bool& Live)
{
	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplSDL2_NewFrame();
	ImGui::NewFrame();

#if 0
	static bool ShowDemoWindow = true;
	ImGui::ShowDemoWindow(&ShowDemoWindow);
#endif

	static bool ShowFocusOverlay = false;
	static bool ShowMeshingStats = false;
	static bool ShowPerformanceStats = false;
	static bool ShowSchedulerStats = false;

	static bool ShowExportOptions = false;
	static float ExportStepSize;
	static float ExportSplitStep[3];
	static float ExportScale;
	static bool ExportSkipRefine;
	static int ExportRefinementSteps;
	static ExportFormat ExportMeshFormat;
	static bool ExportPointCloud = false;
	static float MagicaGridSize = 1.0;
	static int MagicaColorIndex = 0;
	static std::string ExportPath;

	const bool DefaultExportSkipRefine = false;
	const float DefaultExportStepSize = 0.05;
	const float DefaultExportScale = 1.0;
	const int DefaultExportRefinementSteps = 5;
	const float DefaultMagicaGridSize = 0.05;

	if (!HeadlessMode && (ShowDebugInterface || OverrideShowDebugInterface) && ImGui::BeginMainMenuBar())
	{
		if (ImGui::BeginMenu("File"))
		{
			if (ImGui::MenuItem("Open", "Ctrl+O"))
			{
				OpenModel();
			}
			if (ImGui::MenuItem("Reload", "Ctrl+R"))
			{
				ReloadModel();
			}
			if (ImGui::MenuItem("Export As...", nullptr, false, TreeEvaluator != nullptr))
			{
				FlushPendingFileDialogTextureDeletes = false;
				ifd::FileDialog::Instance().Save(
					"ModelExportDialog",
					"Export Model",
					"PLY Model (*.ply){.ply},"
					"STL Model (*.stl){.stl},"
					"Magica Voxel (*.vox){.vox},");
			}
			if (ImGui::MenuItem("Exit"))
			{
				Live = false;
			}
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("View"))
		{
			if (ImGui::BeginMenu("Background"))
			{
				int CurrentMode = GetBackgroundMode();
				if (ImGui::MenuItem("Solid Color", nullptr, CurrentMode == 0))
				{
					UserRequestedBackground = 0;
				}
				if (ImGui::MenuItem("Test Grid", nullptr, CurrentMode == 1))
				{
					UserRequestedBackground = 1;
				}
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Foreground"))
			{
				static MaterialOverride OverrideMode = MaterialOverride::Off;

				bool MaterialOverrideOff = OverrideMode == MaterialOverride::Off;
				if (ImGui::MenuItem("No Override", nullptr, &MaterialOverrideOff))
				{
					OverrideMode = MaterialOverride::Off;
					Sodapop::SetMaterialOverrideMode(OverrideMode);
				}

				bool MaterialOverrideInvariant = OverrideMode == MaterialOverride::Invariant;
				if (ImGui::MenuItem("View Invariant", nullptr, &MaterialOverrideInvariant))
				{
					OverrideMode = MaterialOverride::Invariant;
					Sodapop::SetMaterialOverrideMode(OverrideMode);
				}

				bool MaterialOverrideNormals = OverrideMode == MaterialOverride::Normals;
				if (ImGui::MenuItem("Debug Normals", nullptr, &MaterialOverrideNormals))
				{
					OverrideMode = MaterialOverride::Normals;
					Sodapop::SetMaterialOverrideMode(OverrideMode);
				}

				ImGui::EndMenu();
			}
			if (ImGui::MenuItem("Recenter"))
			{
				ResetCamera = true;
			}
			if (ImGui::MenuItem("Full Screen", "Ctrl+F"))
			{
				ToggleFullScreen(Window);
			}
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Window"))
		{
			if (ImGui::MenuItem("Camera Parameters", nullptr, &ShowFocusOverlay))
			{
			}
			if (ImGui::MenuItem("Performance Stats", nullptr, &ShowPerformanceStats))
			{
			}
			if (ImGui::MenuItem("Scheduler Stats", nullptr, &ShowSchedulerStats))
			{
			}
			if (ImGui::MenuItem("Meshing Stats", nullptr, &ShowMeshingStats))
			{
			}
			ShowStatsOverlay = ShowPerformanceStats || ShowSchedulerStats;
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Help"))
		{
			if (ImGui::MenuItem("Open Source Licenses", nullptr, &ShowLicenses))
			{
			}
			ImGui::EndMenu();
		}

		if (ifd::FileDialog::Instance().IsDone("OpenModelDialog"))
		{
			if (ifd::FileDialog::Instance().HasResult())
			{
				FlushPendingFileDialogTextureDeletes = true;
				const std::vector<std::filesystem::path>& Results = ifd::FileDialog::Instance().GetResults();
				Assert(Results.size() == 1);
				const std::string Path = Results[0].string();
				LoadModel(Path, LanguageForPath(Path));
				LastOpenDir =  Results[0].parent_path();
			}
			ifd::FileDialog::Instance().Close();
		}

		if (ifd::FileDialog::Instance().IsDone("ModelExportDialog"))
		{
			if (ifd::FileDialog::Instance().HasResult())
			{
				FlushPendingFileDialogTextureDeletes = true;
				const std::vector<std::filesystem::path>& Results = ifd::FileDialog::Instance().GetResults();
				Assert(Results.size() == 1);
				ExportPath = Results[0].string();
				ExportMeshFormat = ExportFormatForPath(ExportPath);
				ExportPointCloud = false;
				ShowExportOptions = true;

				const glm::vec3 ModelExtent = ModelBounds.Extent();
				const float ModelVolume = ModelExtent.x * ModelExtent.y * ModelExtent.z;
				const float MinDimension = min(min(ModelExtent.x, ModelExtent.y), ModelExtent.z);
				if (ModelVolume > 0)
				{
					// This is intended to calculate an export resolution that strikes a nice
					// balance between high resolution vs fast, using the model's volume as a
					// rough proxy for export time, so that if a user clicks through the default
					// options they won't be waiting long to get a hopefully useful mesh export.
					const float IdealByMinSide = MinDimension / 10.0;
					const float IdealByVolume = ModelVolume / 2048.0;
					ExportStepSize = min(IdealByMinSide, IdealByVolume);
				}
				else
				{
					// The export is nonsense in this case, so just do whatever.
					ExportStepSize = DefaultExportStepSize;
				}

				for (int i = 0; i < 3; ++i)
				{
					ExportSplitStep[i] = ExportStepSize;
				}
				MagicaGridSize = MagicaGridSize;

				ExportScale = ExportGrid::GetScale();
				if (ExportScale <= 0.0)
				{
					ExportScale = DefaultExportScale;
				}

				ExportSkipRefine = DefaultExportSkipRefine;
				ExportRefinementSteps = DefaultExportRefinementSteps;
			}
			ifd::FileDialog::Instance().Close();
		}

		ImGui::EndMainMenuBar();
	}

	LicenseDisclosuresWindow(ShowLicenses);

	if (ShowFocusOverlay)
	{
		ImGuiWindowFlags WindowFlags = \
			ImGuiWindowFlags_AlwaysAutoResize |
			ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoFocusOnAppearing;

		if (ImGui::Begin("Camera Parameters", &ShowFocusOverlay, WindowFlags))
		{
			ImGui::Text("Focal Point:\n");

			ImGui::Text("X");
			ImGui::SameLine();
			ImGui::InputFloat("##FocusX", &CameraFocus.x, 1.0f);

			ImGui::Text("Y");
			ImGui::SameLine();
			ImGui::InputFloat("##FocusY", &CameraFocus.y, 1.0f);

			ImGui::Text("Z");
			ImGui::SameLine();
			ImGui::InputFloat("##FocusZ", &CameraFocus.z, 1.0f);

			ImGui::Checkbox("Perspective", &UsePerspective);

			ImGui::Text("NearPlane:\n");
			ImGui::InputFloat("##CameraNear", &CameraNear, CameraNear * 0.5);
			CameraNear = max(CameraNear, 0.001);

			if (UsePerspective)
			{
				ImGui::Text("Field of View:\n");
				ImGui::InputFloat("##CameraFov", &CameraFov, 1.0f);
				CameraFov = min(max(CameraFov, 0.001), 180);
			}
			else
			{
				ImGui::Text("FarPlane:\n");
				ImGui::InputFloat("##CameraFar", &CameraFar, CameraFar * 0.5);
				CameraFar = max(CameraFar, CameraNear + 1.0);

				ImGui::Text("Orthographic Scale:\n");
				ImGui::InputFloat("##OrthoScale", &OrthoScale, 16.0);
				OrthoScale = max(OrthoScale, 1.0);
			}
		}
		ImGui::End();
	}

	if (ShowStatsOverlay)
	{
		ImGuiWindowFlags WindowFlags = \
			ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoDecoration |
			ImGuiWindowFlags_AlwaysAutoResize |
			ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoFocusOnAppearing |
			ImGuiWindowFlags_NoNavInputs |
			ImGuiWindowFlags_NoNavFocus |
			ImGuiWindowFlags_NoNav |
			ImGuiWindowFlags_NoInputs;

		const ImGuiViewport* Viewport = ImGui::GetMainViewport();
		ImVec2 Position = Viewport->WorkPos;
		Position.x += 5.0;
		Position.y += 5.0;
		ImVec2 Pivot;
		Pivot.x = 0.0;
		Pivot.y = 0.0;
		ImGui::SetNextWindowPos(Position, ImGuiCond_Always, Pivot);

		if (ImGui::Begin("Performance Stats", &ShowStatsOverlay, WindowFlags))
		{
			if (ShowPerformanceStats)
			{
				{
					ImGui::Text("Cadence\n");
					ImGui::Text(" %.0f hz\n", round(PresentFrequency));
					ImGui::Text(" %.1f ms\n", PresentDeltaMs);
				}
				if (GraphicsBackend == GraphicsAPI::OpenGL4_2)
				{
					ImGui::Separator();
					ImGui::Text("GPU Timeline\n");
					double TotalTimeMs = \
						DepthElapsedTimeMs +
						GridBgElapsedTimeMs +
						UiElapsedTimeMs;
					ImGui::Text("      BG: %.2f ms\n", GridBgElapsedTimeMs);
					ImGui::Text("      FG: %.2f ms\n", DepthElapsedTimeMs);
					ImGui::Text("      UI: %.2f ms\n", UiElapsedTimeMs);
					ImGui::Text("   Total: %.2f ms\n", TotalTimeMs);
				}
				{
					ImGui::Separator();
					ImGui::Text("CPU Timeline\n");
					double TotalTimeMs = \
						TotalDrawTimeMS +
						PresentTimeMs;
					ImGui::Text(" Drawing: %.2f ms\n", TotalDrawTimeMS);
					ImGui::Text(" Present: %.2f ms\n", PresentTimeMs);
					ImGui::Text("   Total: %.2f ms\n", TotalTimeMs);
				}
#if 0
				// TODO : this isn't accurate anymore
				{
					ImGui::Separator();
					ImGui::Text("Model Loading\n");
					ImGui::Text("  Processing: %.3f s\n", ModelProcessingStallMs / 1000.0);
				}
#endif
			}
			if (ShowSchedulerStats)
			{
				Scheduler::Stats(
					SchedulerStats::Inbox,
					SchedulerStats::Outbox,
					SchedulerStats::ParallelQueue,
					SchedulerStats::ContinuousQueue,
					SchedulerStats::DeleteQueue);

				if (ShowPerformanceStats)
				{
					ImGui::Separator();
				}
				ImGui::Text("Scheduler Pressure\n");
				ImGui::Text("      Inbox: %i\n", int(SchedulerStats::Inbox));
				ImGui::Text("     Outbox: %i\n", int(SchedulerStats::Outbox));
				ImGui::Text("   Parallel: %i\n", int(SchedulerStats::ParallelQueue));
				ImGui::Text(" Continuous: %i\n", int(SchedulerStats::ContinuousQueue));
				ImGui::Text("     Delete: %i\n", int(SchedulerStats::DeleteQueue));
			}
		}
		ImGui::End();
	}

	std::vector<std::pair<size_t, DrawableWeakRef>>& DrawableCache = GetDrawableCache();
	if (ShowMeshingStats && DrawableCache.size() > 0)
	{
		ImGuiWindowFlags WindowFlags = \
			ImGuiWindowFlags_HorizontalScrollbar |
			ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoFocusOnAppearing;

		ImGui::SetNextWindowPos(ImVec2(10.0, 32.0), ImGuiCond_Appearing, ImVec2(0.0, 0.0));
		ImGui::SetNextWindowSize(ImVec2(256, 512), ImGuiCond_Appearing);

		if (ImGui::Begin("Meshing Stats", &ShowMeshingStats, WindowFlags))
		{
			for (std::pair<size_t, DrawableWeakRef> Cached : DrawableCache)
			{
				DrawableShared Painter = Cached.second.lock();
				if (Painter)
				{
					if (Painter->MeshAvailable)
					{
						std::string Message = fmt::format("READY: {}", Painter->Name);
						ImGui::TextUnformatted(Message.c_str(), nullptr);

						Message = fmt::format(" - Latency: {} frame(s)", Painter->MeshingFrameLatency);
						ImGui::TextUnformatted(Message.c_str(), nullptr);
					}
					else
					{
						std::string Message = fmt::format("PENDING: {}", Painter->Name);
						ImGui::TextUnformatted(Message.c_str(), nullptr);
					}
				}
				else
				{
					std::string Message = fmt::format("UNEXPECTED: {}", Painter->Name);
					ImGui::TextUnformatted(Message.c_str(), nullptr);
				}
			}
		}
		ImGui::End();
	}
	{
		ExportProgress Progress = GetExportProgress();
		if (Progress.Stage != 0)
		{
			ImVec2 MaxSize = ImGui::GetMainViewport()->WorkSize;
			ImGui::SetNextWindowSizeConstraints(ImVec2(200, 150), MaxSize);
			ImGui::OpenPopup("Export Progress");
			if (ImGui::BeginPopupModal("Export Progress", nullptr, ImGuiWindowFlags_NoSavedSettings))
			{
				if (ExportPointCloud)
				{
					ImGui::ProgressBar(Progress.Generation, ImVec2(-FLT_MIN, 0), "Voxel Search");
					ImGui::ProgressBar(Progress.Refinement, ImVec2(-FLT_MIN, 0), "Point Refinement");
				}
				else
				{
					ImGui::ProgressBar(Progress.Generation, ImVec2(-FLT_MIN, 0), "Naive Surface Nets");
				}
				ImGui::ProgressBar(Progress.Secondary, ImVec2(-FLT_MIN, 0), "Vertex Attributes");
				ImGui::ProgressBar(Progress.Write, ImVec2(-FLT_MIN, 0), "Saving");
				if (ExportPointCloud)
				{
					if (ImGui::Button("Good Enough"))
					{
						CancelExport(false);
					}
					ImGui::SameLine();
				}
				if (ImGui::Button("Halt"))
				{
					CancelExport(true);
				}
				ImGui::EndPopup();
			}
		}
		else if (ShowExportOptions)
		{
			static bool AdvancedOptions = false;
			ImVec2 MaxSize = ImGui::GetMainViewport()->WorkSize;
			ImGui::SetNextWindowSizeConstraints(ImVec2(250, 190), MaxSize);
			ImGui::OpenPopup("Export Options");
			if (ImGui::BeginPopupModal("Export Options", nullptr, ImGuiWindowFlags_NoSavedSettings))
			{
				if (ExportMeshFormat == ExportFormat::VOX)
				{
					ImGui::InputFloat("Voxel Size", &MagicaGridSize);
					ImGui::InputInt("Color Index", &MagicaColorIndex, 1, 10);

					if (ImGui::Button("Start"))
					{
						VoxExport(TreeEvaluator, ExportPath, 1.0 / MagicaGridSize, MagicaColorIndex);
						ShowExportOptions = false;
					}
					ImGui::SameLine();
					if (ImGui::Button("Cancel"))
					{
						ShowExportOptions = false;
					}
				}
				else
				{
					const bool ExportFromSodapop = true;
					// TODO: expose ExportFromSodapop as an option or something
					if (!ExportFromSodapop)
					{
						if (AdvancedOptions)
						{
							ImGui::InputFloat3("Voxel Size", ExportSplitStep);
							ImGui::InputFloat("Unit Scale", &ExportScale);
							ImGui::Checkbox("Skip Refinement", &ExportSkipRefine);
							if (!ExportSkipRefine)
							{
								ImGui::InputInt("Refinement Steps", &ExportRefinementSteps);
							}
						}
						else
						{
							ImGui::InputFloat("Voxel Size", &ExportStepSize);
							ImGui::InputFloat("Unit Scale", &ExportScale);
						}
					}
					if (ExportMeshFormat == ExportFormat::PLY)
					{
						ImGui::Checkbox("Point Cloud Only", &ExportPointCloud);
					}
					if (ImGui::Button("Start"))
					{
						if (ExportFromSodapop)
						{
							MeshExport(ExportPath, true, ExportMeshFormat, ExportScale);
						}
						else if (AdvancedOptions)
						{
							glm::vec3 VoxelSize = glm::vec3(
								ExportSplitStep[0],
								ExportSplitStep[1],
								ExportSplitStep[2]);
							int RefinementSteps = ExportSkipRefine ? 0 : ExportRefinementSteps;
							MeshExport(TreeEvaluator, ExportPath, ModelBounds.Min, ModelBounds.Max, VoxelSize, RefinementSteps, ExportMeshFormat, ExportPointCloud, ExportScale);
						}
						else
						{
							glm::vec3 VoxelSize = glm::vec3(ExportStepSize);
							MeshExport(TreeEvaluator, ExportPath, ModelBounds.Min, ModelBounds.Max, VoxelSize, DefaultExportRefinementSteps, ExportMeshFormat, ExportPointCloud, ExportScale);
						}
						ShowExportOptions = false;
					}
					ImGui::SameLine();
					if (ImGui::Button("Cancel"))
					{
						ShowExportOptions = false;
					}
					ImGui::SameLine();
					if (ImGui::Checkbox("Advanced Options", &AdvancedOptions) && AdvancedOptions)
					{
						for (int i = 0; i < 3; ++i)
						{
							ExportSplitStep[i] = ExportStepSize;
						}
					}
				}
				ImGui::EndPopup();
			}
		}
	}

	if (ScriptErrors.size() > 0)
	{
		std::string ScriptError = ScriptErrors[ScriptErrors.size() - 1];
		{
			ImVec2 TextSize = ImGui::CalcTextSize(ScriptError.c_str(), nullptr);
			TextSize.x += 40.0;
			TextSize.y += 100.0;
			ImVec2 MaxSize = ImGui::GetMainViewport()->WorkSize;
			ImVec2 MinSize(
				TextSize.x < MaxSize.x ? TextSize.x : MaxSize.x,
				TextSize.y < MaxSize.y ? TextSize.y : MaxSize.y);
			ImGui::SetNextWindowSizeConstraints(MinSize, MaxSize);
		}
		{
			ImVec2 Center = ImGui::GetMainViewport()->GetCenter();
			ImGui::SetNextWindowPos(Center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		}
		ImGui::OpenPopup("Error");
		if (ImGui::BeginPopupModal("Error", nullptr, ImGuiWindowFlags_NoSavedSettings))
		{
			{
				ImVec2 Size = ImGui::GetContentRegionAvail();
				Size.y -= 24.0f;
				if (ImGui::BeginChild("ErrorText", Size, false, ImGuiWindowFlags_HorizontalScrollbar))
				{
					ImGui::TextUnformatted(ScriptError.c_str(), nullptr);
				}
				ImGui::EndChild();
			}

			if (ImGui::Button("OK", ImVec2(120, 0)))
			{
				ImGui::CloseCurrentPopup();
				ScriptErrors.pop_back();
			}
			ImGui::SameLine();
			if (ImGui::Button("Copy Error", ImVec2(120, 0)))
			{
				SDL_SetClipboardText(ScriptError.c_str());
			}

			ImGui::EndPopup();
		}
	}
}


void LoadBookmarks()
{
	std::filesystem::path BookmarksPath =
                // FIXME might be read-only
                Installed.ExecutableDir / "bookmarks.txt";
	if (std::filesystem::is_regular_file(BookmarksPath))
	{
		std::ifstream BookmarksFile;
		BookmarksFile.open(BookmarksPath);
		std::string Bookmark;
	repeat:
		std::getline(BookmarksFile, Bookmark);
		if (Bookmark.size() > 0)
		{
			if (std::filesystem::is_directory(Bookmark))
			{
				ifd::FileDialog::Instance().AddFavorite(Bookmark);
			}
			goto repeat;
		}
		BookmarksFile.close();
	}
}


void SaveBookmarks()
{
	std::filesystem::path BookmarksPath =
		// FIXME might be read-only
		Installed.ExecutableDir / "bookmarks.txt";
	const std::vector<std::string>& Bookmarks = ifd::FileDialog::Instance().GetFavorites();
	if (Bookmarks.size() > 0)
	{
		std::ofstream BookmarksFile;
		BookmarksFile.open(BookmarksPath);
		for (const std::string& Bookmark : Bookmarks)
		{
			BookmarksFile << Bookmark << std::endl;
		}
		BookmarksFile.close();
	}
}


enum class DefaultModelLoadingMethod
{
	None,
	ImplicitFileLoad,
	ExplicitFileLoad,
	ExplicitPipeLoad
};


StatusCode LoadDefaultModel(DefaultModelLoadingMethod Method, Language Runtime, std::filesystem::path ModelPath = "")
{
	if (Method == DefaultModelLoadingMethod::None)
	{
		return StatusCode::PASS;
	}
	else if (Method == DefaultModelLoadingMethod::ExplicitPipeLoad)
	{
		if (Runtime == Language::Unknown)
		{
			fmt::print("Reading from stdin requires specifying a language runtime.\n");
			return StatusCode::FAIL;
		}
		else
		{
			LoadFromPipe(Runtime);
			return StatusCode::PASS;
		}
	}
	else
	{
		const bool ExplicitRequest = Method == DefaultModelLoadingMethod::ExplicitFileLoad;

		if (Runtime == Language::Unknown)
		{
			Runtime = LanguageForPath(ModelPath.string());
		}

		if (Runtime == Language::Unknown)
		{
			if (ExplicitRequest)
			{
				fmt::print("Unable to determine the runtime language for loading model {}.\n", ModelPath.string());
				return StatusCode::FAIL;
			}
			else
			{
				return StatusCode::PASS;
			}
		}

		if (std::filesystem::is_regular_file(ModelPath))
		{
			LoadModel(ModelPath.string(), Runtime);
			return StatusCode::PASS;
		}
		else if (ExplicitRequest)
		{
			fmt::print("Unable to read default model from {}.\n", ModelPath.string());
			return StatusCode::FAIL;
		}
	}
}


StatusCode Boot(int argc, char* argv[])
{
	RETURN_ON_FAIL(Installed.PopulateInstallationPaths());
	LastOpenDir = Installed.ModelsDir;
	LoadBookmarks();

	std::vector<std::string> Args;
	for (int i = 1; i < argc; ++i)
	{
		Args.push_back(argv[i]);
	}

#if !_WIN64
	bool RequestSoftwareDriver = false;
#endif
	bool ForceES2 = false;
	bool CreateDebugContext = false;
	bool ForceSingleThread = false;
	VSyncMode RequestedVSyncMode = VSyncMode::Unknown;

	int WindowWidth = 900;
	int WindowHeight = 900;
	HeadlessMode = false;

	// Only knowable after the SDL window has been created.
	int PixelWidth = -1;
	int PixelHeight = -1;

	DefaultModelLoadingMethod InitModelFrom = DefaultModelLoadingMethod::ImplicitFileLoad;
	std::filesystem::path InitModelFilePath = Installed.ModelsDir / "init.lua";
	Language InitModelRuntime = Language::Unknown;
	{
		int Cursor = 0;
		while (Cursor < Args.size())
		{
			if (Args[Cursor] == "--headless" && (Cursor + 2) < Args.size())
			{
				HeadlessMode = true;
				WindowWidth = atoi(Args[Cursor + 1].c_str());
				WindowHeight = atoi(Args[Cursor + 2].c_str());
				Cursor += 3;
				continue;
			}
			else if (Args[Cursor] == "--skipdefault")
			{
				InitModelFrom = DefaultModelLoadingMethod::None;
				Cursor += 1;
				continue;
			}
			else if (Args[Cursor] == "--file")
			{
				InitModelFrom = DefaultModelLoadingMethod::ExplicitFileLoad;
				InitModelFilePath = std::filesystem::path(Args[Cursor + 1].c_str());
				Cursor += 2;
				continue;
			}
			else if (Args[Cursor] == "--pipe")
			{
				InitModelFrom = DefaultModelLoadingMethod::ExplicitPipeLoad;
				Cursor += 1;
				continue;
			}
#if EMBED_LUA
			else if (Args[Cursor] == "--lua")
			{
				InitModelRuntime = Language::Lua;
				Cursor += 1;
				continue;
			}
#endif
#if EMBED_RACKET
			else if (Args[Cursor] == "--racket")
			{
				InitModelRuntime = Language::Racket;
				Cursor += 1;
				continue;
			}
#endif
			else if (Args[Cursor] == "--llvmpipe")
			{
#if _WIN64
				std::cout << "The \"--llvmpipe\" option is only available on Linux.\n";
#else
				RequestSoftwareDriver = true;
#endif
				Cursor += 1;
				continue;
			}
			else if (Args[Cursor] == "--es2")
			{
				ForceES2 = true;
				Cursor += 1;
				continue;
			}
			else if (Args[Cursor] == "--debug-gl")
			{
				CreateDebugContext = true;
				Cursor += 1;
				continue;
			}
			else if (Args[Cursor] == "--msaa" && (Cursor + 1) < Args.size())
			{
				int Hint = glm::clamp(atoi(Args[Cursor + 1].c_str()), 0, 16);
				MultiSampleCount = GLsizei(std::bit_floor(size_t(Hint)));
				Cursor += 2;
				continue;
			}
			else if (Args[Cursor] == "--single-thread")
			{
				ForceSingleThread = true;
				Cursor += 1;
				continue;
			}
			else if (Args[Cursor] == "--adaptive-vsync")
			{
				RequestedVSyncMode = VSyncMode::Adaptive;
				Cursor += 1;
				continue;
			}
			else if (Args[Cursor] == "--no-vsync")
			{
				RequestedVSyncMode = VSyncMode::Disabled;
				Cursor += 1;
				continue;
			}
			else if (Args[Cursor] == "--vsync")
			{
				RequestedVSyncMode = VSyncMode::Enabled;
				Cursor += 1;
				continue;
			}
			else
			{
				std::cout << "Invalid commandline arg(s).\n";
				return StatusCode::FAIL;
			}
		}
	}
	{
#if _DEBUG
		const char* BuildName = "Debug";
#else
		const char* BuildName = "Release";
#endif
#if defined(_MSC_VER)
		fmt::print("{} build [MSVC {}]\n", BuildName, _MSC_VER);
#elif defined(__clang__)
		fmt::print("{} build [clang {}.{}]\n", BuildName, __clang_major__, __clang_minor__);
#elif defined(__GNUC__) || defined(__MINGW32__)
		fmt::print("{} build [GCC {}.{}]\n", BuildName, __GNUC__, __GNUC_MINOR__);
#endif
	}
	Scheduler::Setup(ForceSingleThread);

#if !_WIN64
	Linux::DriverCheck(RequestSoftwareDriver);
#endif
	{
		std::cout << "Setting up SDL2... ";
		SDL_SetMainReady();
		SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
		SDL_SetHint(SDL_HINT_WINDOWS_DPI_SCALING, "1");
		if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER) == 0)
		{
			RETURN_ON_FAIL(BootGL(WindowWidth, WindowHeight, HeadlessMode, ForceES2, CreateDebugContext, RequestedVSyncMode));
		}
		else
		{
			std::cout << "Failed to initialize SDL2.\n";
			return StatusCode::FAIL;
		}

		SDL_GetWindowSizeInPixels(Window, &PixelWidth, &PixelHeight);
	}
	{
		MainEnvironment = new NullEnvironment();
#if EMBED_RACKET
		BootRacket();
#endif
	}
	{
		std::cout << "Setting up Dear ImGui... ";
		std::cout.flush();
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		ImGui::StyleColorsLight();
		ImGuiStyle& Style = ImGui::GetStyle();
		Style.FrameBorderSize = 1.0f;
		ImGui_ImplSDL2_InitForOpenGL(Window, Context);

		if (GraphicsBackend == GraphicsAPI::OpenGLES2)
		{
			ImGui_ImplOpenGL3_Init("#version 100");
		}
		else
		{
			ImGui_ImplOpenGL3_Init("#version 130");
		}

		// Required by ImFileDialog
		{
			ifd::FileDialog::Instance().CreateTexture = [&](uint8_t* Data, int Width, int Height, char Format) -> void*
			{
				bool NeedsDelete = false;
				if (GraphicsBackend == GraphicsAPI::OpenGLES2 && Format == 0)
				{
					// Swizzle BGRA data to RGBA to prevent errors in ES2.

					const size_t Texels = Width * Height;
					{
						// Copy to prevent segfault on Linux.
						const size_t Bytes = Texels * 4;
						Data = (uint8_t*)memcpy(malloc(Bytes), Data, Bytes);
						NeedsDelete = true;
					}

					for (int Texel = 0; Texel < Texels; ++Texel)
					{
						const size_t ByteOffset = Texel * 4;
						uint8_t& B = Data[ByteOffset + 0];
						uint8_t& R = Data[ByteOffset + 2];
						B = B ^ R;
						R = B ^ R;
						B = B ^ R;
					}
					Format = 1;
				}

				GLuint Texture;
				glGenTextures(1, &Texture);
				glBindTexture(GL_TEXTURE_2D, Texture);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, Width, Height, 0, (Format == 0) ? GL_BGRA : GL_RGBA, GL_UNSIGNED_BYTE, Data);
				glGenerateMipmap(GL_TEXTURE_2D);
				glBindTexture(GL_TEXTURE_2D, 0);

				if (NeedsDelete)
				{
					free(Data);
				}
				return (void*)(size_t(Texture));
			};
			ifd::FileDialog::Instance().DeleteTexture = [](void* OpaqueHandle)
			{
				// HACK: ImFileDialog recommends just calling glDeleteTextures here.  If we actually did that,
				// the texture would be deleted while it is still in use, causing icons to get corrupted and
				// GL errors to be logged.  This problem might be due to ImFileDialog being written with
				// assumptions about Dear Imgui that are no longer true.
				// Regardless of why ImFileDialog is like this, we work around the problem by simply letting the
				// textures persist until the frame after the file dialog box has closed.
				GLuint Texture = (GLuint)((uintptr_t)OpaqueHandle);
				PendingFileDialogTextureDeletes.push_back(Texture);
			};
		}

#if _WIN64
		{
			io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 16.0);
			static ImWchar Ranges[] = { 0x1, 0x1FFFF, 0 };
			static ImFontConfig Config;
			Config.OversampleH = 1;
			Config.OversampleV = 1;
			Config.MergeMode = true;
			Config.FontBuilderFlags = 0;
			io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisym.ttf", 16.0, &Config, Ranges);
		}
#endif
		std::cout << "Done!\n";
	}

#ifdef ENABLE_RMLUI
	if (GraphicsBackend == GraphicsAPI::OpenGL4_2)
	{
		std::cout << "Setting up RmlUi... ";
		if (!RmlUiSystemInterface)
		{
			RmlUiSystemInterface = new SystemInterface_SDL();
			RmlUiSystemInterface->SetWindow(Window);
		}
		if (!RmlUiRenderInterface)
		{
			RmlUiRenderInterface = new RenderInterface_GL3();
			RmlUiRenderInterface->SetViewport(WindowWidth, WindowHeight);
		}
		Rml::SetSystemInterface(RmlUiSystemInterface);
		Rml::SetRenderInterface(RmlUiRenderInterface);
		Rml::Initialise();
		RmlUiContext = Rml::CreateContext("Tangerine", Rml::Vector2i(WindowWidth, WindowHeight));
		if (RmlUiContext)
		{
			Rml::Debugger::Initialise(RmlUiContext);

#if 0 // Enable to confirm basic functionality.
			Rml::LoadFontFace("C:\\Windows\\Fonts\\segoeui.ttf");

			const std::string Source = ""
				"<rml><head>\n"
				"<style>\n"
				"body\n"
				"{\n"
				"	font-family: Segoe UI;\n"
				"	font-weight: normal;\n"
				"	font-style: normal;\n"
				"	font-size: 32pt;\n"
				"	font-effect: shadow(2px 2px black);\n"
				"	color: white;\n"
				//"	background-color: rgba(0, 0, 0, 128);\n"
				"	width: 100%;\n"
				"}\n"
				"h1\n"
				"{\n"
				"	font-size: 64pt;\n"
				"	font-weight: bold;\n"
				"}\n"
				"</style>\n"
				"</head><body>\n"
				"	<h1>Hail Eris!</h1><br/>\n"
				"	Hark!  The quick brown fox jumps over the lazy dog!\n"
				"	<br/>The flesh is weak, but the spirit is willing.\n"
				"</body></rml>\n";

			RmlUiDocument = RmlUiContext->LoadDocumentFromMemory(Source);
			RmlUiDocument->Show();
			RmlUiContext->Update();
#endif

			std::cout << "Done!\n";
		}
	}
	else
	{
		std::cout << "Failed to initialize RmlUi.\n";
	}
#endif

	if (SetupRenderer() == StatusCode::FAIL)
	{
		std::cout << "Failed to initialize the renderer.\n";
		return StatusCode::FAIL;
	}

	if (!HeadlessMode)
	{
#if ENABLE_PSMOVE_BINDINGS
		BootPSMove();
#endif
	}

	RETURN_ON_FAIL(LoadDefaultModel(InitModelFrom, InitModelRuntime, InitModelFilePath));

	if (HeadlessMode)
	{
		// There's a frame of delay before an error message would appear, so just process the DearImGui events twice.
		for (int i = 0; i < 2; ++i)
		{
			bool Ignore = true;
			RenderUI(Window, Ignore);
			ImGui::Render();
		}

		// Draw the requested frame or relevant error message.
		{
			MouseMotionX = 45;
			MouseMotionY = 45;

			ViewInfoUpload UploadedView;
			RenderFrame(WindowWidth, WindowHeight, UploadedView);
			ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
			ResolveFrame(PixelWidth, PixelHeight);
			glFinish();
		}

		// Base64 encode the rendered image and dump it to stdout.
		{
			std::vector<unsigned char> PixelData;
			DumpFrameBuffer(WindowWidth, WindowHeight, PixelData);

			std::vector<char> Encoded;
			EncodeBase64(PixelData, Encoded);

			std::cout << "BEGIN RAW IMAGE";
			size_t i = 0;
			for (char& Byte : Encoded)
			{
				std::cout << Byte;
			}
		}
	}
	return StatusCode::PASS;
}


void Teardown()
{
	std::cout << "Shutting down...\n";
	Scheduler::Teardown();

	if (MainEnvironment)
	{
		delete MainEnvironment;
	}
	{
#if ENABLE_PSMOVE_BINDINGS
		TeardownPSMove();
#endif

		if (Context)
		{
#ifdef ENABLE_RMLUI
			if (RmlUiDocument)
			{
				RmlUiDocument->Close();
				RmlUiDocument = nullptr;
			}
			Rml::Shutdown();
			RmlUiContext = nullptr;
			if (RmlUiRenderInterface)
			{
				delete RmlUiRenderInterface;
				RmlUiRenderInterface = nullptr;
			}
			if (RmlUiSystemInterface)
			{
				delete RmlUiSystemInterface;
				RmlUiSystemInterface = nullptr;
			}
#endif
			if (!HeadlessMode)
			{
				SaveBookmarks();
				ImGui_ImplOpenGL3_Shutdown();
				ImGui_ImplSDL2_Shutdown();
				ImGui::DestroyContext();
				ProcessPendingFileDialogTextureDeletes();
			}
		}
		TeardownGL();
		if (Window)
		{
			SDL_DestroyWindow(Window);
		}
	}
}


void MainLoop()
{
	Assert(!HeadlessMode);
	{
		bool Live = true;
		{
			ImGuiIO& io = ImGui::GetIO();
			while (Live)
			{
				BeginEvent("Frame");
				SDL_Event Event;
				MouseMotionX = 0;
				MouseMotionY = 0;
				MouseMotionZ = 0;

				// On high DPI screens, the virtual sizes are the scaled size of the window.
				int VirtualWidth;
				int VirtualHeight;
				SDL_GetWindowSize(Window, &VirtualWidth, &VirtualHeight);

				// On high DPI screens, the pixel sizes represent the real size of the window.
				int PixelWidth;
				int PixelHeight;
				SDL_GetWindowSizeInPixels(Window, &PixelWidth, &PixelHeight);

				const bool HasMouseFocus = Window == SDL_GetMouseFocus();
				static int MouseX = 0;
				static int MouseY = 0;
				if (HasMouseFocus)
				{
					SDL_GetMouseState(&MouseX, &MouseY);
				}

				static ViewInfoUpload LastView;

				static int LastIncompleteCount = 0;
				static int LastRenderableCount = 0;

				static glm::vec3 MouseRay(0.0, 1.0, 0.0);
				static glm::vec3 RayOrigin(0.0, 0.0, 0.0);
				if (HasMouseFocus)
				{
					WorldSpaceRay(LastView, MouseX, MouseY, VirtualWidth, VirtualHeight, RayOrigin, MouseRay);
				}

				static bool LastExportState = false;
				bool ExportInProgress = GetExportProgress().Stage != 0;

				bool RequestDraw = RealtimeMode || ShowStatsOverlay || LastRenderableCount == 0 || LastIncompleteCount > 0 || LastExportState != ExportInProgress || Scheduler::AsyncRedrawRequested();
				LastExportState = ExportInProgress;

#if ENABLE_RMLUI
				bool RmlUiActive = RmlUiContext && RmlUiContext->GetNumDocuments() > 0;
#endif

				if (!ShowDebugInterface)
				{
					SDL_Keymod ModState = SDL_GetModState();
					OverrideShowDebugInterface = (ModState & KMOD_ALT) != 0;
				}

				BeginEvent("Process Input");
				while (SDL_PollEvent(&Event))
				{
					ImGui_ImplSDL2_ProcessEvent(&Event);

					if (Event.type == SDL_QUIT ||
						(Event.type == SDL_WINDOWEVENT && Event.window.event == SDL_WINDOWEVENT_CLOSE && Event.window.windowID == SDL_GetWindowID(Window)))
					{
						Live = false;
						break;
					}
					else
					{
						RequestDraw = true;
					}
#if ENABLE_RMLUI
					if (RmlUiActive)
					{
						const bool ContinuePropagating = RmlSDL::InputEventHandler(RmlUiContext, Event);
						if (!ContinuePropagating)
						{
							RmlUiContext->Update();
						}
					}
#endif
					static bool Dragging = false;
					if (Dragging && FixedCamera)
					{
						Dragging = false;
						SDL_SetRelativeMouseMode(SDL_FALSE);
					}
					if (!io.WantCaptureMouse && HasMouseFocus && LastRenderableCount > 0)
					{
						switch (Event.type)
						{
						case SDL_MOUSEMOTION:
							if (Dragging)
							{
								MouseMotionX = Event.motion.xrel;
								MouseMotionY = Event.motion.yrel;
							}
							else
							{
								DeliverMouseMove(RayOrigin, MouseRay, Event.motion.x, Event.motion.y);
							}
							break;
						case SDL_MOUSEBUTTONDOWN:
							if (DeliverMouseButton(MouseEvent(Event.button, RayOrigin, MouseRay)) && !FixedCamera)
							{
								Dragging = true;
								SDL_SetRelativeMouseMode(SDL_TRUE);
							}
							break;
						case SDL_MOUSEBUTTONUP:
							if (Dragging)
							{
								Dragging = false;
								SDL_SetRelativeMouseMode(SDL_FALSE);
							}
							else
							{
								DeliverMouseButton(MouseEvent(Event.button, RayOrigin, MouseRay));
							}
							break;
						case SDL_MOUSEWHEEL:
							if (DeliverMouseScroll(RayOrigin, MouseRay, Event.wheel.x, Event.wheel.y))
							{
								MouseMotionZ = Event.wheel.y;
							}
							break;
						}
					}
					else if (Dragging && ScriptErrors.size() > 0)
					{
						Dragging = false;
						SDL_SetRelativeMouseMode(SDL_FALSE);
					}
					if (!io.WantCaptureKeyboard && Event.type == SDL_KEYDOWN)
					{
						const int SHIFT_FLAG = 1 << 9;
						const int CTRL_FLAG = 1 << 10;
						const int ALT_FLAG = 1 << 11;
						const int OPEN_MODEL = CTRL_FLAG | SDLK_o;
						const int RELOAD_MODEL = CTRL_FLAG | SDLK_r;
						const int TOGGLE_FULLSCREEN = CTRL_FLAG | SDLK_f;
						const int TOGGLE_LICENSE_INFO = SDLK_F1;
						int Key = Event.key.keysym.sym;
						int Mod = Event.key.keysym.mod;
						if ((Mod & KMOD_SHIFT) != 0)
						{
							Key |= SHIFT_FLAG;
						}
						if ((Mod & KMOD_CTRL) != 0)
						{
							Key |= CTRL_FLAG;
						}
						if ((Mod & KMOD_ALT) != 0)
						{
							Key |= ALT_FLAG;
						}
						switch (Key)
						{
						case OPEN_MODEL:
							OpenModel();
							break;
						case RELOAD_MODEL:
							ReloadModel();
							break;
						case TOGGLE_FULLSCREEN:
							ToggleFullScreen(Window);
							break;
						case TOGGLE_LICENSE_INFO:
							ShowLicenses = not ShowLicenses;
							break;
						case SDLK_KP_MULTIPLY:
							MouseMotionZ += 5;
							break;
						case SDLK_KP_DIVIDE:
							MouseMotionZ -= 5;
							break;
						case SDLK_KP_1: // ⭩
							MouseMotionX += 45;
							MouseMotionY -= 45;
							break;
						case SDLK_KP_2: // ⭣
							MouseMotionY -= 45;
							break;
						case SDLK_KP_3: // ⭨
							MouseMotionX -= 45;
							MouseMotionY -= 45;
							break;
						case SDLK_KP_4: // ⭠
							MouseMotionX += 45;
							break;
						case SDLK_KP_6: // ⭢
							MouseMotionX -= 45;
							break;
						case SDLK_KP_7: // ⭦
							MouseMotionX += 45;
							MouseMotionY += 45;
							break;
						case SDLK_KP_8: // ⭡
							MouseMotionY += 45;
							break;
						case SDLK_KP_9: // ⭧
							MouseMotionX -= 45;
							MouseMotionY += 45;
							break;
						}
					}
					RouteControllerEvents(Event);
				}
				EndEvent();

				if (MainEnvironment->CanAdvance)
				{
					BeginEvent("Advance");
					double DeltaTime;
					double ElapsedTime;
					{
						ProfilingTimePoint CurrentTimePoint = ProfilingClock::now();
						static ProfilingTimePoint OriginTimePoint = CurrentTimePoint;
						static ProfilingTimePoint LastTimePoint = CurrentTimePoint;
						{
							std::chrono::duration<double, std::milli> FrameDelta = CurrentTimePoint - LastTimePoint;
							DeltaTime = double(FrameDelta.count());
						}
						{
							std::chrono::duration<double, std::milli> EpochDelta = CurrentTimePoint - OriginTimePoint;
							ElapsedTime = double(EpochDelta.count());
						}
						LastTimePoint = CurrentTimePoint;
					}
					MainEnvironment->Advance(DeltaTime, ElapsedTime);
					RequestDraw = true;
					EndEvent();
				}

				if (RequestDraw || ExportInProgress)
				{
					{
						BeginEvent("Update UI");
						RenderUI(Window, Live);
						ImGui::Render();
						EndEvent();
					}
					{
						PaintingSet::GatherModelStats(LastIncompleteCount, LastRenderableCount);
					}
					{
						RenderFrame(PixelWidth, PixelHeight, LastView);
					}
#ifdef ENABLE_RMLUI
					if (RmlUiActive)
					{
						BeginEvent("RmlUi Draw");
						glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, -1, "RmlUi");
						RmlUiRenderInterface->SetViewport(ScreenWidth, ScreenHeight);
						RmlUiRenderInterface->BeginFrame();

						RmlUiContext->Render();
						RmlUiRenderInterface->EndFrame();
						glPopDebugGroup();
						EndEvent();
						SetPipelineDefaults();
					}
#endif
					{
						BeginEvent("Dear ImGui Draw");
						glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, -1, "Dear ImGui");
						UiTimeQuery.Start();
						ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
						UiTimeQuery.Stop();
						glPopDebugGroup();
						EndEvent();
					}
					{
						ProfilingTimePoint StartTime = ProfilingClock::now();
						BeginEvent("Present");
						ResolveFrame(PixelWidth, PixelWidth);
						SDL_GL_SwapWindow(Window);
						EndEvent();
						std::chrono::duration<double, std::milli> PresentDelta = ProfilingClock::now() - StartTime;
						PresentTimeMs = PresentDelta.count();
					}
					{
						BeginEvent("Query Results");
						DepthElapsedTimeMs = DepthTimeQuery.ReadMs();
						GridBgElapsedTimeMs = GridBgTimeQuery.ReadMs();
						UiElapsedTimeMs = UiTimeQuery.ReadMs();

						EndEvent();
					}
					if (FlushPendingFileDialogTextureDeletes)
					{
						ProcessPendingFileDialogTextureDeletes();
					}
				}
				{
					BeginEvent("Scheduler::Advance");
					Scheduler::Advance();
					EndEvent();
				}
				EndEvent();
			}
		}
	}
}


#ifndef MINIMAL_DLL
int main(int argc, char* argv[])
{
	if (Boot(argc, argv) == StatusCode::PASS)
	{
		MainLoop();
	}
	Teardown();
	return 0;
}
#endif

