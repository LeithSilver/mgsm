#include "overlay.h"
#include "assetviewer.h"
#include "settings.h"
#include "textures.h"
#include "custommodel.h"
#include "gametools.h"
#include "photomode.h"

#include "glheaders.h"
#include "gles.h"
#include "runtime.h"
#include "touchcontrols.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <string>

#include "imgui.h"
#ifdef MGS_GLES
#include "imgui_impl_gles1.h"
#define ImGui_ImplOpenGL2_Init ImGui_ImplGLES1_Init
#define ImGui_ImplOpenGL2_NewFrame() ((void)0)
#define ImGui_ImplOpenGL2_RenderDrawData ImGui_ImplGLES1_RenderDrawData
#else
#include "backends/imgui_impl_opengl2.h"
#endif

namespace {
enum EvType { EV_MOVE, EV_BUTTON, EV_WHEEL, EV_KEY, EV_CHAR };

ImGuiKey VKToImGuiKey(int vk) {
    if (vk >= '0' && vk <= '9') return static_cast<ImGuiKey>(ImGuiKey_0 + (vk - '0'));
    if (vk >= 'A' && vk <= 'Z') return static_cast<ImGuiKey>(ImGuiKey_A + (vk - 'A'));
    if (vk >= VK_F1 && vk <= VK_F12) return static_cast<ImGuiKey>(ImGuiKey_F1 + (vk - VK_F1));
    switch (vk) {
        case VK_TAB: return ImGuiKey_Tab;           case VK_LEFT: return ImGuiKey_LeftArrow;
        case VK_RIGHT: return ImGuiKey_RightArrow;  case VK_UP: return ImGuiKey_UpArrow;
        case VK_DOWN: return ImGuiKey_DownArrow;    case VK_PRIOR: return ImGuiKey_PageUp;
        case VK_NEXT: return ImGuiKey_PageDown;     case VK_HOME: return ImGuiKey_Home;
        case VK_END: return ImGuiKey_End;           case VK_INSERT: return ImGuiKey_Insert;
        case VK_DELETE: return ImGuiKey_Delete;     case VK_BACK: return ImGuiKey_Backspace;
        case VK_SPACE: return ImGuiKey_Space;       case VK_RETURN: return ImGuiKey_Enter;
        case VK_ESCAPE: return ImGuiKey_Escape;     case VK_SHIFT: return ImGuiKey_LeftShift;
        case VK_CONTROL: return ImGuiKey_LeftCtrl;  case VK_MENU: return ImGuiKey_LeftAlt;
        default: return ImGuiKey_None;
    }
}

// Controller as a mouse (SDL button ids): A, R1 and R2 click; the d-pad moves the cursor like the left stick.
constexpr int kPadClick[] = { 0, 10, 27 };
constexpr int kPadUp = 11, kPadDown = 12, kPadLeft = 13, kPadRight = 14;

using PFN_ActiveTexture = void (APIENTRY*)(GLenum);
constexpr auto kWarningShowTime = std::chrono::seconds(10);   // on-screen time of a new warning (debug mode)
constexpr size_t kMaxWarnings = 64;
constexpr GLenum GL_TEXTURE0_ = 0x84C0, GL_ACTIVE_TEXTURE_ = 0x84E0, GL_CLIENT_ACTIVE_TEXTURE_ = 0x84E1;

// The 3DS menu leaves out what doesn't apply there: settings its ApplySettings fixes (main_3ds.cpp), the keyboard,
// the touch controls (the touch screen is this menu's mouse), photo mode and the asset viewers (no framebuffers).
// The Vita's (kVita, 960x544: the desktop layout fits) leaves out the same settings, the keyboard, the touch
// controls (not on the Vita yet; the front touch screen is this menu's mouse), photo mode and the asset viewers.
#ifdef MGS_3DS
constexpr bool k3DS = true;
#else
constexpr bool k3DS = false;
#endif
#ifdef MGS_VITA
constexpr bool kVita = true;
#else
constexpr bool kVita = false;
#endif
constexpr bool kConsole = k3DS || kVita;

ImGuiStyle g_base_style;   // the style at 1x; Render scales it with the render resolution

// A setting's description: grey, indented under the control and wrapped to the window, with a little space after.
void Hint(const char* fmt, ...) IM_FMTARGS(1);
void Hint(const char* fmt, ...) {
    ImGui::Indent();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    va_list args;
    va_start(args, fmt);
    ImGui::TextWrappedV(fmt, args);
    va_end(args);
    ImGui::PopStyleColor();
    ImGui::Unindent();
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));
}

// A titled group of settings, with room above it.
void Section(const char* title) {
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));
    ImGui::SeparatorText(title);
}
}

Overlay& Overlay::Instance() {
    static Overlay inst;
    return inst;
}

void Overlay::SetOpen(bool open) {
    if (!open) { capture_kind_ = 0; capture_action_ = -1; }
    open_ = open;
}

void Overlay::OnMouseMove(float x, float y) { std::lock_guard<std::mutex> l(mutex_); events_.push_back({EV_MOVE, 0, 0, x, y}); }
void Overlay::OnMouseButton(int b, bool down) { std::lock_guard<std::mutex> l(mutex_); events_.push_back({EV_BUTTON, b, down, 0, 0}); }
void Overlay::OnMouseWheel(float d) { std::lock_guard<std::mutex> l(mutex_); events_.push_back({EV_WHEEL, 0, 0, d, 0}); }
void Overlay::OnKey(int vk, bool down) { std::lock_guard<std::mutex> l(mutex_); events_.push_back({EV_KEY, vk, down, 0, 0}); }
void Overlay::OnChar(unsigned c) { std::lock_guard<std::mutex> l(mutex_); events_.push_back({EV_CHAR, static_cast<int>(c), 0, 0, 0}); }

void Overlay::OnCaptureKey(int vk) {
    if (capture_kind_ != 1) return;
    if (vk == VK_F1 || vk == VK_F5 || vk == VK_F6 || vk == VK_F9 || vk == VK_F11 || vk == VK_F12) return;   // app shortcuts, never sent to the game
    captured_ = (vk == VK_ESCAPE) ? -2 : vk;   // -2 = cancelled
}

void Overlay::OnPad(const Gamepad::Raw& raw) {
    std::lock_guard<std::mutex> l(mutex_);
    if (capture_kind_ == 2 && pad_valid_) {   // first newly pressed button (Back is reserved)
        for (int b = 0; b < PAD_BUTTON_COUNT; ++b)
            if (raw.buttons[b] && !pad_.buttons[b] && !Gamepad::Reserved(b)) { captured_ = b; break; }
    }
    pad_ = raw;
    pad_valid_ = true;
}

void Overlay::AddWarning(const std::string& text) {
    std::lock_guard<std::mutex> l(warnings_mutex_);
    if (warnings_.size() >= kMaxWarnings) warnings_.erase(warnings_.begin());
    warnings_.push_back({text, std::chrono::steady_clock::now(), false});
}

void Overlay::AddNotice(const std::string& text) {
    std::lock_guard<std::mutex> l(warnings_mutex_);
    if (warnings_.size() >= kMaxWarnings) warnings_.erase(warnings_.begin());
    warnings_.push_back({text, std::chrono::steady_clock::now(), true});
}

bool Overlay::WarningsVisible() {   // notices always, warnings in debug mode
    const bool debug = Settings::Instance().debug_mode;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> l(warnings_mutex_);
    for (const Warning& w : warnings_)
        if ((debug || w.notice) && now - w.time < kWarningShowTime) return true;
    return false;
}

// Controller as a mouse while the overlay is open: the left stick (squared response) or the d-pad moves the
// cursor, A / R1 / R2 are the left button, the right stick scrolls. mutex_ held.
void Overlay::PadCursor(ImGuiIO& io, int fb_w, int fb_h) {
    const float dt = (std::min)(io.DeltaTime, 0.1f);
    if (cursor_x_ < 0.0f) { cursor_x_ = fb_w * 0.5f; cursor_y_ = fb_h * 0.5f; }
    auto curve = [](float v) {
        const float dead = 0.18f, a = std::fabs(v);
        if (a < dead) return 0.0f;
        const float t = (a - dead) / (1.0f - dead);
        return v < 0 ? -t * t : t * t;
    };
    float dx = curve(pad_.lx), dy = curve(pad_.ly);
    const bool* b = pad_.buttons;
    if (b[kPadLeft]) dx -= 0.6f;
    if (b[kPadRight]) dx += 0.6f;
    if (b[kPadUp]) dy -= 0.6f;
    if (b[kPadDown]) dy += 0.6f;
    const float speed = fb_h * 1.1f;   // pixels per second at full deflection
    if (dx != 0.0f || dy != 0.0f) {
        cursor_x_ = (std::max)(0.0f, (std::min)(fb_w - 1.0f, cursor_x_ + dx * speed * dt));
        cursor_y_ = (std::max)(0.0f, (std::min)(fb_h - 1.0f, cursor_y_ + dy * speed * dt));
        io.AddMousePosEvent(cursor_x_, cursor_y_);
        pad_cursor_ = true;
    }
    bool click = false, was = false;
    for (int c : kPadClick) { click = click || b[c]; was = was || pad_prev_.buttons[c]; }
    if (click != was) {
        if (click && !pad_cursor_) { io.AddMousePosEvent(cursor_x_, cursor_y_); pad_cursor_ = true; }
        io.AddMouseButtonEvent(0, click);
    }
    const float scroll = curve(pad_.ry);
    if (scroll != 0.0f) io.AddMouseWheelEvent(0.0f, -scroll * 12.0f * dt);   // wheel notches per second
}

void Overlay::Render(int fb_w, int fb_h) {
    if (fb_w <= 0 || fb_h <= 0) return;
    const bool open = open_;
    const bool photo = PhotoMode::Instance().Active();
    TouchControls& touch = TouchControls::Instance();
    if (!open && !photo && !WarningsVisible() && !touch.Visible()) return;
    touch.SetMenuAvailable(!S3ERuntime::Instance().CanChangeRoom());   // menus, the codec, the pause menu
    static PFN_ActiveTexture p_active = reinterpret_cast<PFN_ActiveTexture>(GLES::Proc("glActiveTexture"));
    static PFN_ActiveTexture p_client = reinterpret_cast<PFN_ActiveTexture>(GLES::Proc("glClientActiveTexture"));

    if (!initialised_) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;   // our settings live in mgs_mobile.ini
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;   // the controller drives a cursor instead (PadCursor)
        ImGui::StyleColorsDark();
        ImGuiStyle& st = ImGui::GetStyle();
        st.WindowRounding = 6.0f; st.FrameRounding = 4.0f; st.GrabRounding = 4.0f;
        st.WindowPadding = ImVec2(18.0f, 14.0f);   // room between the settings and the window's edges
        st.FramePadding = ImVec2(8.0f, 4.0f);
        st.ItemSpacing = ImVec2(10.0f, 6.0f);
        st.ItemInnerSpacing = ImVec2(8.0f, 4.0f);
        st.IndentSpacing = 28.0f;
        st.SeparatorTextPadding = ImVec2(0.0f, 6.0f);
        st.TabRounding = 4.0f;
        g_base_style = st;
        st.Colors[ImGuiCol_WindowBg].w = 0.94f;
        ImGui_ImplOpenGL2_Init();
        initialised_ = true;
    }
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(fb_w), static_cast<float>(fb_h));
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    io.DeltaTime = (std::max)(1e-4f, std::chrono::duration<float>(now - last).count());
    last = now;
    // Scale the UI with the render resolution (1.0 at 480 px tall): text and spacing alike.
    const float scale = (std::max)(1.0f, fb_h / 480.0f);
    if (scale != ImGui::GetStyle().FontScaleMain) {
        ImGuiStyle& st = ImGui::GetStyle();
        st = g_base_style;
        st.ScaleAllSizes(scale);
        st.FontScaleMain = scale;
    }

    {   // input
        std::lock_guard<std::mutex> l(mutex_);
        size_t done = 0;
        const int frame = ImGui::GetFrameCount();
        bool moved = false;
        for (const Event& e : events_) {
            // A tap (touch screen) jumps the pointer, presses and releases within a frame or two. ImGui only takes a
            // press on an item that was already hovered the frame before, so a press waits a frame after a pointer
            // move, and the release waits until the press is 3 frames old.
            if (e.type == EV_MOVE) moved = true;
            if (e.type == EV_BUTTON && e.a >= 0 && e.a < 3) {
                if (e.b != 0 && moved) break;
                if (e.b == 0 && frame - press_frame_[e.a] < 3) break;
                if (e.b != 0) press_frame_[e.a] = frame;
            }
            ++done;
            switch (e.type) {
                case EV_MOVE: io.AddMousePosEvent(e.x, e.y); cursor_x_ = e.x; cursor_y_ = e.y; pad_cursor_ = false; break;
                case EV_BUTTON: io.AddMouseButtonEvent(e.a, e.b != 0); break;
                case EV_WHEEL: io.AddMouseWheelEvent(0.0f, e.x); break;
                case EV_KEY: {
                    ImGuiKey k = VKToImGuiKey(e.a);
                    if (k != ImGuiKey_None) io.AddKeyEvent(k, e.b != 0);
                    if (e.a == VK_CONTROL) io.AddKeyEvent(ImGuiMod_Ctrl, e.b != 0);
                    if (e.a == VK_SHIFT) io.AddKeyEvent(ImGuiMod_Shift, e.b != 0);
                    if (e.a == VK_MENU) io.AddKeyEvent(ImGuiMod_Alt, e.b != 0);
                    break;
                }
                case EV_CHAR: io.AddInputCharacter(static_cast<unsigned>(e.a)); break;
            }
        }
        events_.erase(events_.begin(), events_.begin() + static_cast<std::ptrdiff_t>(done));   // the rest next frame
        if (open && pad_valid_ && capture_kind_ != 2) PadCursor(io, fb_w, fb_h);
        pad_prev_ = pad_;
        io.MouseDrawCursor = open && pad_cursor_;   // ImGui draws the cursor while the controller moves it
    }

    // Apply a captured key/button to the binding being edited (each input drives one action only).
    const int cap = captured_.exchange(-1);
    if (cap != -1 && capture_kind_ != 0) {
        const int action = capture_action_;
        const bool pad = capture_kind_ == 2;
        if (cap >= 0 && action >= 0) {
            auto b = pad ? Settings::Instance().ControllerBindings() : Settings::Instance().KeyboardBindings();
            for (auto& list : b) list.erase(std::remove(list.begin(), list.end(), cap), list.end());
            b[action].push_back(cap);
            if (pad) Settings::Instance().SetControllerBindings(b); else Settings::Instance().SetKeyboardBindings(b);
            if (apply_) apply_();
        }
        capture_kind_ = 0;
        capture_action_ = -1;
    }

    // Draw on top of the finished game frame, leaving the game's GL state exactly as it was.
    GLint active = GL_TEXTURE0_, client = GL_TEXTURE0_;
    glGetIntegerv(GL_ACTIVE_TEXTURE_, &active);
    glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE_, &client);
    {
    GLStateSave save;
    if (p_active) { p_active(GL_TEXTURE0_ + 1); glDisable(GL_TEXTURE_2D); p_active(GL_TEXTURE0_); }
    if (p_client) p_client(GL_TEXTURE0_);
    glMatrixMode(GL_TEXTURE); glPushMatrix(); glLoadIdentity();
    glDisable(GL_FOG); glDisable(GL_ALPHA_TEST);
    glDisable(GL_LIGHTING);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    ImGui_ImplOpenGL2_NewFrame();
    ImGui::NewFrame();
    if (open) {
        Draw();
        AssetViewer::RenderPreview();   // the Models tab's 3D view, into its texture before ImGui draws it
        if (touch_tab_) touch.Draw(ImGui::GetForegroundDrawList(), fb_w, fb_h, true);   // live preview while editing
    } else {
        if (!photo) touch.Draw(ImGui::GetBackgroundDrawList(), fb_w, fb_h, false);
        if (WarningsVisible()) DrawWarnings();   // (Render also runs for the touch controls alone)
        if (photo) DrawPhotoHint();
    }
    ImGui::Render();
    ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());

    glMatrixMode(GL_TEXTURE); glPopMatrix();
    }
    if (p_active) p_active(static_cast<GLenum>(active));
    if (p_client) p_client(static_cast<GLenum>(client));
}

void Overlay::Draw() {
    Settings& s = Settings::Instance();
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 size(std::min(io.DisplaySize.x * 0.92f, 760.0f * ImGui::GetStyle().FontScaleMain),
                      std::min(io.DisplaySize.y * 0.92f, 620.0f * ImGui::GetStyle().FontScaleMain));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    bool keep_open = true;
    if (touch_tab_) ImGui::SetNextWindowBgAlpha(0.6f);   // the touch controls show through while they're edited
    if (!ImGui::Begin(kConsole ? "Settings   -   Select to close" : "Settings   -   F1 / Back / R3 to close", &keep_open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                                                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        return;
    }
    bool changed = false;
    // Debug: MGS_VIEWER=textures, models[:group] or file:<group file> opens that tab the first time (models also
    // captures a frame, file: loads a group file).
    static const char* viewer_env = getenv("MGS_VIEWER");
    ImGuiTabItemFlags textures_flags = 0, models_flags = 0;
    if (viewer_env && s.debug_mode) {
        const std::string v = viewer_env;
        if (v.rfind("file:", 0) == 0) {
            models_flags = ImGuiTabItemFlags_SetSelected;
            for (size_t at = 5; at <= v.size();) {   // file:a,b,c loads each in turn
                const size_t comma = (std::min)(v.find(',', at), v.size());
                AssetViewer::RequestLoad(v.substr(at, comma - at));
                at = comma + 1;
            }
        } else if (v.rfind("models", 0) == 0) {
            models_flags = ImGuiTabItemFlags_SetSelected;
            AssetViewer::RequestCapture(v.size() > 7 ? atoi(v.c_str() + 7) : 0);
        } else if (v == "textures") {
            textures_flags = ImGuiTabItemFlags_SetSelected;
        }
        viewer_env = nullptr;
    }
    if (ImGui::BeginTabBar("tabs")) {
        touch_tab_ = false;
        // Each tab scrolls on its own; the footer (where settings are saved) stays at the bottom.
        const float footer_h = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2.0f;
        auto begin_page = [&] { return ImGui::BeginChild("page", ImVec2(0, -footer_h), ImGuiChildFlags_None); };
        const float combo_w = 200.0f * ImGui::GetStyle().FontScaleMain;
        if (ImGui::BeginTabItem("Display")) {
            if (begin_page()) {
            Section(k3DS ? "Draw Soft Keys" : "Screen");
            bool v = s.fullscreen;
#ifndef __ANDROID__   // (always fullscreen there)
            if (!kConsole && ImGui::Checkbox("Fullscreen  (F11 / Alt+Enter)", &v)) { s.fullscreen = v; changed = true; }
#endif
            if (!k3DS) {
            v = s.arbitrary_resolution;
            if (ImGui::Checkbox("Arbitrary resolution", &v)) { s.arbitrary_resolution = v; changed = true; }
            Hint("On: the game fills the window at any aspect ratio, 3D view to the edges. "
                 "Off: the phone's 240x320 / 320x240 screen, letterboxed.");
            }
            int sk = s.hide_soft_keys;
            static const char* kSoftKeyModes[] = { "Show", "Smart: hide in gameplay (Equip / Codec)", "All: hide everywhere" };
            // (3DS: the section title names it, and the combo takes the narrow screen's whole width)
            ImGui::SetNextItemWidth(k3DS ? -FLT_MIN : 320.0f * ImGui::GetStyle().FontScaleMain);
            if (ImGui::Combo(k3DS ? "##softkeys" : "Soft key labels", &sk, kSoftKeyModes, 3)) { s.hide_soft_keys = sk; changed = true; }
            Hint("Smart keeps the menu / codec / inventory labels (Select, Skip, Close).");

            Section("Image quality");
            if (!kConsole) {
            static const int kMsaaValues[] = { 0, 2, 4, 8 };
            static const char* kMsaaNames[] = { "Off", "2x", "4x", "8x" };
            int mi = 0;
            for (int i = 0; i < 4; ++i) if (kMsaaValues[i] == s.msaa) mi = i;
            ImGui::SetNextItemWidth(combo_w);
            if (ImGui::Combo("Anti-aliasing (MSAA)", &mi, kMsaaNames, 4)) { s.msaa = kMsaaValues[mi]; changed = true; }
            Hint("Smooths polygon edges.");
            static const char* kSsNames[] = { "1x (off)", "2x", "3x" };
            int si = (std::max)(1, (std::min)(3, s.supersample.load())) - 1;
            ImGui::SetNextItemWidth(combo_w);
            if (ImGui::Combo("Supersampling", &si, kSsNames, 3)) { s.supersample = si + 1; changed = true; }
            Hint("Renders at 2x / 3x the window resolution and scales down. Smooths everything, much heavier on the GPU.");
            }
            v = s.texture_filtering;
            if (ImGui::Checkbox(kConsole ? "Bilinear texture filtering" : "Bilinear texture filtering  (F9)", &v)) { s.texture_filtering = v; changed = true; }
            Hint("Off matches the phone (sharp pixels).");
            if (!kConsole) {
            v = s.native_resolution;
            if (ImGui::Checkbox("Native resolution (pixelated)", &v)) { s.native_resolution = v; changed = true; }
            Hint("Renders at the phone's resolution (240 px tall) and scales it up with sharp pixels, like the "
                 "software renderer. Supersampling is ignored; MSAA still applies.");
            }
            v = s.custom_textures;
            if (ImGui::Checkbox("Custom textures", &v)) { s.custom_textures = v; changed = true; }
            Hint("PNGs edited or added in the textures folder (%d found at startup). Applies as textures load (next room).",
                 TexturePack::ReplacementCount());

            Section(k3DS ? "Field of view" : "Camera and world");
            int fov = s.fov_percent;
            if (k3DS)   // (the slider fills the line up to the Reset button)
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Reset").x -
                                        ImGui::GetStyle().FramePadding.x * 2.0f - ImGui::GetStyle().ItemSpacing.x);
            else
                ImGui::SetNextItemWidth(260.0f * ImGui::GetStyle().FontScaleMain);
            if (ImGui::SliderInt(k3DS ? "##fov" : "Field of view", &fov, 50, 150, "%d%%")) { s.fov_percent = fov; changed = true; }
            ImGui::SameLine();
            if (ImGui::Button("Reset##fov")) { s.fov_percent = 100; changed = true; }
            Hint("100%% = the game's own camera. Higher shows more of the scene.");
            if (!k3DS) {
            v = s.draw_all_geometry;
            if (ImGui::Checkbox("Draw all level geometry", &v)) { s.draw_all_geometry = v; changed = true; }
            Hint("Off: the phone's per-camera culling (walls pop in at the edges of wide views). On: the whole room is drawn.");
            }

            if (CustomModel::FolderPresent()) {   // only for players who have a customModel folder (nothing shipped)
            Section("Player model");
            ImGui::BeginDisabled(!CustomModel::Loaded());
            v = s.custom_player_model;
            if (ImGui::Checkbox("Custom player model", &v)) { s.custom_player_model = v; changed = true; }
            ImGui::EndDisabled();
            if (CustomModel::Loaded())
                Hint("Replaces Snake with customModel/snake.mgsm (%d triangles), animated by the game's own skeleton.",
                     CustomModel::TriangleCount());
            else
                Hint("Not available: %s. Make it with tools/custommodel/convert.py.", CustomModel::Error().c_str());
            }

            if (!kConsole) {   // (the software renderer isn't presented by the 3DS's GL layer; not tried on the Vita)
            Section("Renderer");
            v = s.software_renderer;
            if (ImGui::Checkbox("Software renderer (restart)", &v)) { s.software_renderer = v; changed = true; }
            Hint("The game's own CPU rasteriser at the phone's resolution, scaled up. Anti-aliasing and custom "
                 "textures don't apply; filtering smooths the upscale. Takes effect on restart.");
            if (s.software_renderer != !S3ERuntime::UseGL())
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Restart the game to switch renderers.");
            }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (!kConsole && ImGui::BeginTabItem("Keyboard")) {
            if (begin_page()) DrawBindings(false);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (!k3DS && ImGui::BeginTabItem("Controller")) {   // (3DS: fixed buttons; the bindings didn't fit the screen)
            if (begin_page()) {
                bool v = s.left_stick_dpad;
                if (ImGui::Checkbox(k3DS ? "Circle pad moves Snake" : "Left stick moves Snake", &v)) { s.left_stick_dpad = v; changed = true; }
                if (k3DS)
                    Hint("Select opens and closes this menu. In the menu the circle pad or d-pad moves the cursor, "
                         "A / R / ZR click, the C-stick scrolls; the touch screen works too.");
                else if (kVita)
                    Hint("Select opens and closes this menu. In the menu the left stick or d-pad moves the cursor, "
                         "Cross / R click, the right stick scrolls; the touch screen works too.");
                else
                    Hint("Back / Select and R3 (right stick click) open and close this menu. In the menu the left stick or "
                         "d-pad moves the cursor, A / R1 / R2 click, the right stick scrolls.");
                DrawBindings(true);
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (!kConsole && ImGui::BeginTabItem("Touch")) {
            touch_tab_ = true;
            if (begin_page()) DrawTouch(changed);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(k3DS ? "Enhancements" : "General")) {
            if (begin_page()) DrawGeneral(changed);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (s.debug_mode && ImGui::BeginTabItem("Inventory")) { DrawInventory(changed); ImGui::EndTabItem(); }
        if (s.debug_mode && ImGui::BeginTabItem("Level Select")) {
            if (DrawLevelSelect()) keep_open = false;
            ImGui::EndTabItem();
        }
        if (!kConsole && s.debug_mode && ImGui::BeginTabItem("Textures", nullptr, textures_flags)) { AssetViewer::DrawTexturesTab(); ImGui::EndTabItem(); }
        if (!kConsole && s.debug_mode && ImGui::BeginTabItem("Models", nullptr, models_flags)) { AssetViewer::DrawModelsTab(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::Separator();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Saved automatically to %s", s.Path().c_str());
    ImGui::End();

    if (capture_kind_ != 0) {
        ImGui::OpenPopup("capture");
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("capture", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
            const int a = capture_action_;
            ImGui::Text("%s", capture_kind_ == 1 ? "Press a key for:" : "Press a controller button for:");
            ImGui::Text("    %s", a >= 0 ? PhoneActions()[a].label : "?");
            ImGui::TextDisabled("%s", capture_kind_ == 1 ? "Esc to cancel (F1 / F5 / F6 / F9 / F11 / F12 are reserved)"
                                      : k3DS ? "Cancel on the touch screen" : kVita ? "Cancel below (Cross or touch)"
                                      : "Cancel with the mouse or Esc");
            if (capture_kind_ == 2 && (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))) { capture_kind_ = 0; capture_action_ = -1; }
            if (capture_kind_ == 0) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
    if (changed && apply_) apply_();
    if (!keep_open && close_) close_();
}

void Overlay::DrawTouch(bool& changed) {
    Settings& s = Settings::Instance();
    int mode = s.touch_mode;
    const char* modes[] = { "Off", "Auto", "On" };
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    if (ImGui::Combo("Touch controls", &mode, modes, 3)) { s.touch_mode = mode; changed = true; }
    Hint("Auto: shown once you touch the screen, hidden again when a controller or keyboard is used. "
         "The menu button at the top middle (menus, the codec, the pause menu) opens this menu.");
    int opacity = s.touch_opacity;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    if (ImGui::SliderInt("Opacity", &opacity, 10, 100, "%d%%")) { s.touch_opacity = opacity; changed = true; }
    Section("Layout");
    Hint("Position (centre, %% of the screen) and size (%% of the screen height).");
    for (int c = 0; c < TouchControls::COUNT; ++c) {
        ImGui::PushID(c);
        ImGui::Text("%s", TouchControls::Label(c));
        const float w = ImGui::GetFontSize() * 7;
        float x = s.touch_x[c] * 100, y = s.touch_y[c] * 100, size = s.touch_size[c] * 100;
        ImGui::SetNextItemWidth(w);
        if (ImGui::SliderFloat("X", &x, 0, 100, "%.0f%%")) { s.touch_x[c] = x / 100; changed = true; }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(w);
        if (ImGui::SliderFloat("Y", &y, 0, 100, "%.0f%%")) { s.touch_y[c] = y / 100; changed = true; }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(w);
        if (ImGui::SliderFloat("Size", &size, 5, 90, "%.0f%%")) { s.touch_size[c] = size / 100; changed = true; }
        ImGui::PopID();
    }
    if (ImGui::Button("Reset layout")) { s.ResetTouchLayout(); changed = true; }
}

void Overlay::DrawGeneral(bool& changed) {
    Settings& s = Settings::Instance();
    Section("Startup");
    bool v = s.skip_intro;
    if (ImGui::Checkbox("Skip intro logos", &v)) { s.skip_intro = v; changed = true; }
    Hint("Boot straight to \"Press any key\", past the Konami, Kojima Productions and Ideaworks3D screens.");
    Section("Gameplay");
    v = s.diagonal_movement;
    if (ImGui::Checkbox("Diagonal movement", &v)) { s.diagonal_movement = v; changed = true; }
    Hint("Two directions at once move Snake diagonally (the game has it built in, switched off). "
         "Off: the phone's four directions (up / down wins over left / right).");
    v = s.analog_movement;
    if (ImGui::Checkbox("Analog movement", &v)) { s.analog_movement = v; changed = true; }
    Hint(k3DS ? "The circle pad steers Snake at any angle, not just 8 directions."
              : kVita ? "The left stick steers Snake at any angle, not just 8 directions."
              : "The left stick and the touch D-pad steer Snake at any angle, not just 8 directions.");
    if (!kConsole) {   // (the consoles suspend the whole app for their home menus and sleep)
    v = s.pause_on_focus_loss;
    if (ImGui::Checkbox("Pause when the window loses focus", &v)) { s.pause_on_focus_loss = v; changed = true; }
    Hint("Like an incoming call on the phone: the game freezes while another window is active and opens its "
         "pause menu when you come back (in gameplay).");
    }
    Section("Debug");
    v = s.debug_mode;
    if (ImGui::Checkbox("Debug mode", &v)) { s.debug_mode = v; changed = true; }
    Hint("Level select tab, graphics warnings on screen, verbose console log.");
    if (!s.debug_mode) return;
    if (!kConsole) {
    Section("Photo mode");
    ImGui::BeginDisabled(!can_load_ || !can_load_());
    if (ImGui::Button(PhotoMode::Instance().Active() ? "Leave photo mode" : "Enter photo mode  (F5)") && photo_) photo_();
    ImGui::EndDisabled();
    Hint("Freezes the game and frees the camera (gameplay only). Controller: left stick move, right stick look, "
         "LT / RT down / up, LB fast, RB step a frame, Y screenshot, X reset, B exit, d-pad up / down zoom, "
         "d-pad left show / hide the HUD.");
    Hint("F12 saves a screenshot at the render resolution to the screenshots folder (any time).");
    }
    Section("Original debug commands");
    Hint("Left in the game's code and scripts by the developers.");
    v = s.dbg_show_fps;
    if (ImGui::Checkbox("Show frame rate", &v)) { s.dbg_show_fps = v; changed = true; }
    Hint("IwGx's own counter (GX/DisplayFramerate), top left.");
    v = s.dbg_lua_debug_mode;
    if (ImGui::Checkbox("Script debug mode", &v)) { s.dbg_lua_debug_mode = v; changed = true; }
    Hint("InDebugMode(): the scripts' Assert / Trace messages (console; failed asserts also on screen). "
         "The scripts read it once at startup: restart the game after changing it.");
    v = s.dbg_frame_rate_test;
    if (ImGui::Checkbox("Frame rate test start", &v)) { s.dbg_frame_rate_test = v; changed = true; }
    Hint("IsFrameRateTest(): a new game skips the opening cutscene and starts Snake at the Utility Room's test spawn point.");
    Section("Warnings and script messages");
    std::vector<Warning> list;
    { std::lock_guard<std::mutex> l(warnings_mutex_); list = warnings_; }
    if (list.empty()) { ImGui::TextDisabled("None so far."); return; }
    if (ImGui::BeginChild("warnings", ImVec2(0, 200.0f * ImGui::GetStyle().FontScaleMain), ImGuiChildFlags_Borders))
        for (const Warning& w : list) ImGui::TextWrapped("%s", w.text.c_str());
    ImGui::EndChild();
}

// Recent warnings in the top-left corner, over the game (debug mode, overlay closed). No input.
void Overlay::DrawWarnings() {
    std::vector<Warning> list;
    { std::lock_guard<std::mutex> l(warnings_mutex_); list = warnings_; }
    const auto now = std::chrono::steady_clock::now();
    const Settings& s = Settings::Instance();
    const float below_fps = (s.debug_mode && s.dbg_show_fps) ? ImGui::GetIO().DisplaySize.y * 0.07f : 0.0f;   // IwGx's counter
    ImGui::SetNextWindowPos(ImVec2(8.0f, 8.0f + below_fps), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.6f);
    if (ImGui::Begin("##warnings", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav)) {
        for (const Warning& w : list)
            if ((s.debug_mode || w.notice) && now - w.time < kWarningShowTime)
                ImGui::TextColored(w.notice ? ImVec4(0.6f, 1.0f, 0.6f, 1.0f) : ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", w.text.c_str());
    }
    ImGui::End();
}

// Photo mode controls, bottom left (drawn after the screenshot is read, so never in it).
void Overlay::DrawPhotoHint() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(8.0f, io.DisplaySize.y - 8.0f), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    if (ImGui::Begin("##photo", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav)) {
        const PhotoMode& pm = PhotoMode::Instance();
        ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "PHOTO MODE  (time frozen, speed %.0f, FOV %.0f%%, HUD %s)",
                           pm.Speed(), pm.Fov(), pm.HudHidden() ? "hidden" : "shown");
        ImGui::TextUnformatted("WASD / left stick move   Q E / LT RT down, up   mouse drag / arrows / right stick look");
        ImGui::TextUnformatted("Shift / LB fast   Ctrl slow   wheel speed   Z X / d-pad up down zoom   H / d-pad left HUD");
        ImGui::TextUnformatted("R / X reset camera and FOV");
        ImGui::TextUnformatted("F6 / RB step one frame   F12 / Y screenshot   F5 / Esc / B exit");
    }
    ImGui::End();
}

// Debug mode: cheats, health, cardkey, story stage and the inventory (GameTools). Values come from the last
// snapshot; edits apply in a game frame let through for them, then the snapshot refreshes.
void Overlay::DrawInventory(bool& changed) {
    Settings& s = Settings::Instance();
    GameTools& gt = GameTools::Instance();
    const GameTools::Snapshot snap = gt.Get();
    ImGui::SeparatorText("Cheats");
    bool v = s.cheat_infinite_health;
    if (ImGui::Checkbox("Infinite health", &v)) { s.cheat_infinite_health = v; changed = true; }
    ImGui::SameLine();
    v = s.cheat_infinite_ammo;
    if (ImGui::Checkbox("Infinite ammo", &v)) { s.cheat_infinite_ammo = v; changed = true; }
    Hint("Applied in gameplay while debug mode is on (saved with the settings).");
    if (!snap.valid) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "The rest is available in gameplay only.");
        return;
    }
    ImGui::SeparatorText("Snake");
    ImGui::Text("Health %d / %d", snap.health, snap.max_health);
    ImGui::SameLine();
    if (ImGui::SmallButton("Refill")) gt.RefillHealth();
    int key = (std::max)(1, (std::min)(GameTools::kMaxCardkey, snap.story.cardkey)) - 1;   // combo index = level - 1
    static const char* kKeys[GameTools::kMaxCardkey] = { "Level 1", "Level 2", "Level 3", "Level 4" };
    ImGui::SetNextItemWidth(140.0f * ImGui::GetStyle().FontScaleMain);
    if (ImGui::Combo("Cardkey", &key, kKeys, GameTools::kMaxCardkey)) gt.SetCardkey(key + 1);
    const int stage = snap.story.valid ? snap.story.CurrentStage() : 0;
    ImGui::Text("Story: %s", Story::StageName(stage));
    static int advance = -1;
    if (advance <= stage) advance = (std::min)(stage + 1, Story::STAGE_COUNT - 1);
    if (stage + 1 < Story::STAGE_COUNT) {
        ImGui::SetNextItemWidth(260.0f * ImGui::GetStyle().FontScaleMain);
        if (ImGui::BeginCombo("##advance", Story::StageName(advance))) {
            for (int i = stage + 1; i < Story::STAGE_COUNT; ++i)
                if (ImGui::Selectable(Story::StageName(i), advance == i)) advance = i;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Advance story")) gt.AdvanceStory(advance);
    }
    ImGui::SeparatorText("Inventory");
    const auto& items = GameTools::Items();
    if (ImGui::BeginTable("inv", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthFixed, 200.0f * ImGui::GetStyle().FontScaleMain);
        ImGui::TableSetupColumn("Amount", ImGuiTableColumnFlags_WidthFixed, 150.0f * ImGui::GetStyle().FontScaleMain);
        ImGui::TableSetupColumn("");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            const bool owned = i < static_cast<int>(snap.owned.size()) && snap.owned[i];
            const bool equipped = i < static_cast<int>(snap.equipped.size()) && snap.equipped[i];
            const int amount = i < static_cast<int>(snap.amount.size()) ? snap.amount[i] : 0;
            const int step = items[i].weapon && items[i].give >= 10 ? 10 : 1;
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            if (owned) ImGui::Text("%s%s", items[i].label, equipped ? "  (equipped)" : "");
            else ImGui::TextDisabled("%s", items[i].label);
            ImGui::TableSetColumnIndex(1);
            if (owned) {
                if (ImGui::SmallButton("-")) gt.SetAmount(i, amount - step);
                ImGui::SameLine();
                ImGui::Text("%4d", amount);
                ImGui::SameLine();
                if (ImGui::SmallButton("+")) gt.SetAmount(i, amount + step);
                ImGui::SameLine();
                if (ImGui::SmallButton("Max")) gt.Fill(i);
            }
            ImGui::TableSetColumnIndex(2);
            if (!owned) {
                if (ImGui::SmallButton("Give")) gt.Give(i);
            } else {
                ImGui::BeginDisabled(equipped);
                if (ImGui::SmallButton("Remove")) gt.Remove(i);
                ImGui::EndDisabled();
                if (equipped && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Unequip it in the game first");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// Every facility room (chapters/facility/<name>.group.bin); the name is what the game's RoomChange takes.
// Prefix 01 = ground level, 02 = basement 1 (e.g. "B1 South Corridor"), 03 = basement 2.
bool Overlay::DrawLevelSelect() {
    // {room, a neighbour to arrive from} - neighbours from the rooms' scripts (their Pa/Na exits).
    struct Room { const char* name; const char* via; };
    static const Room kRooms[] = {
        {"01_Utility_Room", "01_Storeroom"}, {"01_Utility_Control_Room", "01_Base_Main_Entrance"},
        {"01_Storeroom", "01_Utility_Room"}, {"01_Equipment_Store", "01_Storeroom"},
        {"01_Base_Main_Entrance", "01_Storeroom"}, {"01_Primary_Walkway", "01_Base_Main_Entrance"},
        {"01_Auxiliary_Corridor", "01_Primary_Walkway"},
        {"02_South_Corridor", "01_Auxiliary_Corridor"}, {"02_Mess_Hall", "02_South_Corridor"},
        {"02_East_West_Corridor", "02_Mess_Hall"}, {"02_Toilets", "02_East_West_Corridor"},
        {"02_Bunk_Room", "02_Toilets"}, {"02_Central_Corridor", "02_Toilets"},
        {"02_Security_Control_Room", "02_East_West_Corridor"}, {"02_Lift_Access", "02_Security_Control_Room"},
        {"02_Armoury", "02_Security_Control_Room"}, {"02_Temporary_Storage", "02_Armoury"},
        {"02_Surveillance_Tunnel", "02_Central_Corridor"}, {"02_Control_Room", "02_Central_Corridor"},
        {"02_Observation_Area", "02_Control_Room"},
        {"03_Marine_Supplies", "02_Observation_Area"}, {"03_Basement", "03_Marine_Supplies"},
        {"03_Old_Storeroom", "03_Basement"}, {"03_Door_Mechanism_Room", "03_Basement"},
        {"03_Metal_Gear", "03_Door_Mechanism_Room"},
    };
    static const char* kFloors[] = { "Ground level", "Basement 1", "Basement 2" };
    const bool can = can_load_ && can_load_();
    // The 3DS's 240 px screen: a short note instead of the paragraph, so the room list gets the height (the tab
    // itself doesn't scroll, so anything taller than the screen was cut off).
    if (!k3DS)
        ImGui::TextWrapped("Jump to any room of the facility, the same way a door does. The story is brought forward to "
                           "where you would first reach the room (never back); Snake keeps his items (see the Inventory tab). "
                           "Save first if you care about the current file: rooms autosave when you enter them.");
    if (!can) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), k3DS ? "Gameplay only: start or resume a game first."
                                                                     : "Available in gameplay only: start or resume a game, then open this menu.");
    const GameTools::Snapshot snap = GameTools::Instance().Get();
    const int stage = snap.valid && snap.story.valid ? snap.story.CurrentStage() : -1;
    if (stage >= 0) ImGui::Text("Story: %s", Story::StageName(stage));
    const float list_h = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * (k3DS ? 1.5f : 2.5f);
    bool chosen = false;
    if (ImGui::BeginChild("rooms", ImVec2(0, (std::max)(list_h, k3DS ? 60.0f : 120.0f)), ImGuiChildFlags_Borders)) {
        int floor = -1;
        for (int i = 0; i < static_cast<int>(sizeof(kRooms) / sizeof(kRooms[0])); ++i) {
            const int f = kRooms[i].name[1] - '1';
            if (f != floor) { floor = f; ImGui::SeparatorText(kFloors[(std::max)(0, (std::min)(2, f))]); }
            std::string label = std::string(kRooms[i].name + 3);
            std::replace(label.begin(), label.end(), '_', ' ');
            if (ImGui::Selectable((label + "##" + kRooms[i].name).c_str(), level_selected_ == i, ImGuiSelectableFlags_AllowDoubleClick)) {
                level_selected_ = i;
                if (can && ImGui::IsMouseDoubleClicked(0)) chosen = true;
            }
        }
    }
    ImGui::EndChild();
    ImGui::BeginDisabled(!can || level_selected_ < 0);
    if (ImGui::Button("Load room")) chosen = true;
    ImGui::EndDisabled();
    if (level_selected_ >= 0) {
        ImGui::SameLine();
        const int target = stage >= 0 ? Story::TargetStage(kRooms[level_selected_].name, stage) : -1;
        if (k3DS) {   // (one line: whether the story moves)
            if (target > stage) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Story advances");
            else if (target >= 0) ImGui::TextDisabled("Story unchanged");
        } else {
            ImGui::TextDisabled("%s  (arriving from %s)", kRooms[level_selected_].name, kRooms[level_selected_].via);
            if (target > stage) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Story advances to: %s", Story::StageName(target));
            else if (target >= 0) ImGui::TextDisabled("Story unchanged");
        }
    }
    if (chosen && can && level_selected_ >= 0 && load_room_) { load_room_(kRooms[level_selected_].name, kRooms[level_selected_].via); return true; }
    return false;
}

void Overlay::DrawBindings(bool controller) {
    Settings& s = Settings::Instance();
    auto b = controller ? s.ControllerBindings() : s.KeyboardBindings();
    bool modified = false;
    if (ImGui::Button(controller ? "Reset controller to defaults" : "Reset keyboard to defaults")) {
        if (controller) s.ResetController(); else s.ResetKeyboard();
        if (apply_) apply_();
        return;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("+ adds a %s; click a binding to remove it.", controller ? "button" : "key");
    if (ImGui::BeginTable("bind", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Phone button", ImGuiTableColumnFlags_WidthFixed, 230.0f * ImGui::GetStyle().FontScaleMain);
        ImGui::TableSetupColumn(controller ? "Controller" : "Keyboard");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        const auto& actions = PhoneActions();
        for (size_t i = 0; i < actions.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(actions[i].label);
            ImGui::TableSetColumnIndex(1);
            for (size_t j = 0; j < b[i].size(); ++j) {
                ImGui::PushID(static_cast<int>(j));
                std::string name = (controller ? PadButtonName(b[i][j]) : VKName(b[i][j])) + "  x";
                if (ImGui::SmallButton(name.c_str())) { b[i].erase(b[i].begin() + j); modified = true; ImGui::PopID(); break; }
                ImGui::PopID();
                ImGui::SameLine();
            }
            if (ImGui::SmallButton("+")) { capture_action_ = static_cast<int>(i); captured_ = -1; capture_kind_ = controller ? 2 : 1; }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (modified) {
        if (controller) s.SetControllerBindings(b); else s.SetKeyboardBindings(b);
        if (apply_) apply_();
    }
}
