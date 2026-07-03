// SDL2 + OpenGL3 + Dear ImGui bootstrap for the sonview GUI.
#include <SDL.h>
#include <SDL_opengl.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "app.h"
#include "icon_data.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl2.h"
#include "imgui_internal.h"  // DockBuilder* for the default layout

namespace son {

static std::string layout_ini_path() {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    std::string base = xdg && *xdg
                           ? xdg
                           : std::string(getenv("HOME") ? getenv("HOME") : ".") + "/.config";
    return base + "/sonview/layout.ini";
}

int run_gui(const std::string &default_host, bool autocapture, bool autoconnect) {
    // Match the .desktop StartupWMClass so the running window groups with the
    // pinned launcher entry (X11 WM_CLASS). Don't override a user's setting.
    setenv("SDL_VIDEO_X11_WMCLASS", "sonview", 0);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    // GL 3.0 + GLSL 130
    const char *glsl_version = "#version 130";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#ifdef SDL_HINT_IME_SHOW_UI
    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    SDL_WindowFlags window_flags =
        (SDL_WindowFlags)(SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Window *window = SDL_CreateWindow("sonview", SDL_WINDOWPOS_CENTERED,
                                          SDL_WINDOWPOS_CENTERED, 1400, 900, window_flags);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    // window/taskbar icon (embedded RGBA; regenerate via scripts/gen-icon.sh)
    SDL_Surface *icon = SDL_CreateRGBSurfaceWithFormatFrom(
        (void *)ICON_RGBA, ICON_W, ICON_H, 32, ICON_W * 4, SDL_PIXELFORMAT_RGBA32);
    if (icon) {
        SDL_SetWindowIcon(window, icon);
        SDL_FreeSurface(icon);
    }

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    if (!gl_context) {
        std::fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // Persist the user's layout tweaks across launches; View -> "Reset window
    // layout" rebuilds the default docking arrangement.
    static std::string ini_path = layout_ini_path();
    io.IniFilename = ini_path.c_str();
    ImGui::StyleColorsDark();

    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init(glsl_version);

    App app;
    // Only an explicit --connect overrides the persisted host: launching plain
    // `sonview` must come up with the IP you used last time.
    if (!default_host.empty()) app.set_default_host(default_host);
    if (autocapture) app.enable_autopilot();
    if (autoconnect) app.request_connect();

    bool done = false;
    int title_cooldown = 0;
    while (!done && !app.want_quit()) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) done = true;
            if (event.type == SDL_WINDOWEVENT &&
                event.window.event == SDL_WINDOWEVENT_CLOSE &&
                event.window.windowID == SDL_GetWindowID(window))
                done = true;
        }
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(10);
            continue;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        // reflect connection/capture state in the OS window title
        if (--title_cooldown <= 0) {
            title_cooldown = 30;
            SDL_SetWindowTitle(window, app.window_title().c_str());
        }

        ImGuiID dsid = ImGui::DockSpaceOverViewport();
        // Build the default layout only when no saved layout exists (first run)
        // or the user asked for a reset.
        bool have_layout = ImGui::DockBuilderGetNode(dsid) != nullptr &&
                           ImGui::DockBuilderGetNode(dsid)->IsSplitNode();
        if (!have_layout || app.consume_layout_reset()) {
            ImGui::DockBuilderRemoveNode(dsid);
            ImGui::DockBuilderAddNode(dsid, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dsid, ImGui::GetMainViewport()->WorkSize);
            ImGuiID center = dsid;
            ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.24f, nullptr, &center);
            ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.26f, nullptr, &center);
            ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.22f, nullptr, &center);
            ImGuiID scope = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.34f, nullptr, &center);
            ImGui::DockBuilderDockWindow("Scope", scope);
            ImGui::DockBuilderDockWindow("Connection", left);
            ImGui::DockBuilderDockWindow("Device", left);
            ImGui::DockBuilderDockWindow("Decoders", left);
            ImGui::DockBuilderDockWindow("Capture", left);
            ImGui::DockBuilderDockWindow("Waveform", center);
            ImGui::DockBuilderDockWindow("Markers", right);
            ImGui::DockBuilderDockWindow("Measurements", right);
            ImGui::DockBuilderDockWindow("Log", bottom);
            ImGui::DockBuilderDockWindow("Decoded", bottom);
            ImGui::DockBuilderFinish(dsid);
        }
        app.draw_ui();

        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0.09f, 0.09f, 0.11f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

}  // namespace son
