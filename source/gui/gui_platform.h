#pragma once
// MAC-3: the thin GUI's window, renderer and OS dialogs - one implementation per OS (gui_platform_win32_dx11.cpp:
// Win32 + D3D11, gui_platform_mac.mm: GLFW + Metal). thin_gui.cpp is the same on both.
#include <string>
#include <vector>

namespace GuiPlatform {

// The emulator is a real-time load: Windows - 1 ms timer granularity, no EcoQoS throttling.
void beginRealtime();
void endRealtime();

bool createWindow(const char* title, int width, int height);   // the window and its graphics device
std::vector<std::string> panelFontFiles();                     // the vector panel's legend font, best first
void initBackends();                                           // after ImGui::CreateContext and the fonts
bool pollEvents();                                             // false once the window is closed
void newFrame();                                               // the backends' NewFrame; ImGui::NewFrame follows
// After ImGui::Render: draw and present. shotPath != nullptr: also save this frame (the window's own pixels,
// never the rest of the screen) as a 32-bit BMP.
void render(const float clear[4], const char* shotPath);
void shutdownBackends();                                       // before ImGui::DestroyContext
void destroyWindow();

// A .syx file to open (saving = false) or to write (saving = true); false if cancelled.
bool fileDialog(bool saving, std::string& out);

// macOS only (MS2000R.app started from Finder; on Windows the .bat launchers start in the MS2000 folder):
bool chooseFolder(const char* message, std::string& out);      // a folder; false if cancelled
void alert(const char* title, const char* text);

} // namespace GuiPlatform
