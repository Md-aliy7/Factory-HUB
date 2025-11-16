// GatewayUI.h
#pragma once
// --- Dear ImGui Includes ---
#include <GL/glew.h>
#include <GL/gl.h>
#include <GLFW/glfw3.h>
#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#ifdef _WIN32
#include <GLFW/glfw3native.h>
#endif

class GatewayHub; // Forward declaration (tells the compiler "this name exists")

// This is OK because it only uses a reference
void DrawGatewayUI(GatewayHub& hub);

