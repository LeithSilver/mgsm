// Dear ImGui renderer for OpenGL ES 1.1 (fixed function), for the overlay on Android. Adapted from
// third_party/imgui/backends/imgui_impl_opengl2.cpp (MIT, Omar Cornut): the same drawing, minus what ES lacks
// (attribute stack, polygon mode, GL_UNPACK_ROW_LENGTH). The caller saves and restores the GL state
// (GLStateSave in overlay.cpp); this only pushes and pops the matrices it replaces.
#ifdef MGS_GLES
#include "imgui_impl_gles1.h"
#include "glheaders.h"
#include "imgui.h"
#include <cstddef>
#include <cstdint>

namespace {

void UpdateTexture(ImTextureData* tex) {
    if (tex->Status == ImTextureStatus_WantCreate) {
        IM_ASSERT(tex->Format == ImTextureFormat_RGBA32);
        GLuint id = 0;
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex->Width, tex->Height, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex->GetPixels());
        tex->SetTexID(static_cast<ImTextureID>(static_cast<intptr_t>(id)));
        tex->SetStatus(ImTextureStatus_OK);
    } else if (tex->Status == ImTextureStatus_WantUpdates) {
        // No row length in ES 1.1: each updated rectangle goes up one row at a time
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(static_cast<intptr_t>(tex->TexID)));
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        for (const ImTextureRect& r : tex->Updates)
            for (int y = 0; y < r.h; ++y)
                glTexSubImage2D(GL_TEXTURE_2D, 0, r.x, r.y + y, r.w, 1, GL_RGBA, GL_UNSIGNED_BYTE, tex->GetPixelsAt(r.x, r.y + y));
        tex->SetStatus(ImTextureStatus_OK);
    } else if (tex->Status == ImTextureStatus_WantDestroy) {
        GLuint id = static_cast<GLuint>(static_cast<intptr_t>(tex->TexID));
        glDeleteTextures(1, &id);
        tex->SetTexID(ImTextureID_Invalid);
        tex->SetStatus(ImTextureStatus_Destroyed);
    }
}

void SetupRenderState(ImDrawData* dd, int fb_w, int fb_h) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_LIGHTING);
    glDisable(GL_COLOR_MATERIAL);
    glEnable(GL_SCISSOR_TEST);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glEnable(GL_TEXTURE_2D);
    glShadeModel(GL_SMOOTH);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glViewport(0, 0, fb_w, fb_h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(dd->DisplayPos.x, dd->DisplayPos.x + dd->DisplaySize.x, dd->DisplayPos.y + dd->DisplaySize.y, dd->DisplayPos.y, -1.0f, 1.0f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

} // namespace

bool ImGui_ImplGLES1_Init() {
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "imgui_impl_gles1";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    return true;
}

void ImGui_ImplGLES1_Shutdown() {
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
        if (tex->RefCount == 1) { tex->SetStatus(ImTextureStatus_WantDestroy); UpdateTexture(tex); }
}

void ImGui_ImplGLES1_RenderDrawData(ImDrawData* dd) {
    const int fb_w = static_cast<int>(dd->DisplaySize.x * dd->FramebufferScale.x);
    const int fb_h = static_cast<int>(dd->DisplaySize.y * dd->FramebufferScale.y);
    if (fb_w <= 0 || fb_h <= 0) return;
    if (dd->Textures)
        for (ImTextureData* tex : *dd->Textures)
            if (tex->Status != ImTextureStatus_OK) UpdateTexture(tex);

    glMatrixMode(GL_PROJECTION); glPushMatrix();
    glMatrixMode(GL_MODELVIEW); glPushMatrix();
    SetupRenderState(dd, fb_w, fb_h);
    const ImVec2 off = dd->DisplayPos, scale = dd->FramebufferScale;
    for (const ImDrawList* list : dd->CmdLists) {
        const ImDrawVert* vtx = list->VtxBuffer.Data;
        const ImDrawIdx* idx = list->IdxBuffer.Data;
        glVertexPointer(2, GL_FLOAT, sizeof(ImDrawVert), reinterpret_cast<const char*>(vtx) + offsetof(ImDrawVert, pos));
        glTexCoordPointer(2, GL_FLOAT, sizeof(ImDrawVert), reinterpret_cast<const char*>(vtx) + offsetof(ImDrawVert, uv));
        glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(ImDrawVert), reinterpret_cast<const char*>(vtx) + offsetof(ImDrawVert, col));
        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            if (cmd.UserCallback) {
                if (cmd.UserCallback == ImDrawCallback_ResetRenderState) SetupRenderState(dd, fb_w, fb_h);
                else cmd.UserCallback(list, &cmd);
                continue;
            }
            const ImVec2 mn((cmd.ClipRect.x - off.x) * scale.x, (cmd.ClipRect.y - off.y) * scale.y);
            const ImVec2 mx((cmd.ClipRect.z - off.x) * scale.x, (cmd.ClipRect.w - off.y) * scale.y);
            if (mx.x <= mn.x || mx.y <= mn.y) continue;
            glScissor(static_cast<int>(mn.x), static_cast<int>(fb_h - mx.y), static_cast<int>(mx.x - mn.x), static_cast<int>(mx.y - mn.y));
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(static_cast<intptr_t>(cmd.GetTexID())));
            static_assert(sizeof(ImDrawIdx) == 2, "ES 1.1 draws 16-bit indices only");
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(cmd.ElemCount), GL_UNSIGNED_SHORT, idx + cmd.IdxOffset);
        }
    }
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glMatrixMode(GL_MODELVIEW); glPopMatrix();
    glMatrixMode(GL_PROJECTION); glPopMatrix();
}
#endif
