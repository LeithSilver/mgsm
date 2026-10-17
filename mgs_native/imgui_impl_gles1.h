#pragma once

// Dear ImGui renderer for OpenGL ES 1.1 (MGS_GLES builds; desktop uses imgui_impl_opengl2). See the .cpp.
struct ImDrawData;
bool ImGui_ImplGLES1_Init();
void ImGui_ImplGLES1_Shutdown();
void ImGui_ImplGLES1_RenderDrawData(ImDrawData* draw_data);
