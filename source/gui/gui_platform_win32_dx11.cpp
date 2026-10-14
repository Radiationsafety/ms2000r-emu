// MAC-3: gui_platform.h on Windows - Win32 window, D3D11 swap chain, the common file dialogs. Moved here from
// thin_gui.cpp unchanged.
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <tchar.h>
#include <commdlg.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "winmm.lib")
#include <timeapi.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "gui_platform.h"
#include <cstdio>
#include <cstdint>
#include <vector>

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {
ID3D11Device*           g_dev = nullptr;
ID3D11DeviceContext*    g_ctx = nullptr;
IDXGISwapChain*         g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
HWND                    g_hwnd = nullptr;
WNDCLASSEX              g_wc{};

void createRT()  { ID3D11Texture2D* bb = nullptr; g_swap->GetBuffer(0, IID_PPV_ARGS(&bb)); if (bb) { g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv); bb->Release(); } }
void cleanupRT() { if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; } }
bool createDevice(HWND h)
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = h;
    sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL fl[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    return D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fl, 2, D3D11_SDK_VERSION,
                                         &sd, &g_swap, &g_dev, &got, &g_ctx) == S_OK;
}
void cleanupDevice()
{
    cleanupRT();
    if (g_swap) { g_swap->Release(); g_swap = nullptr; }
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
}
LRESULT WINAPI wndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return true;
    if (m == WM_SIZE && g_dev && w != SIZE_MINIMIZED) {
        cleanupRT(); g_swap->ResizeBuffers(0, LOWORD(l), HIWORD(l), DXGI_FORMAT_UNKNOWN, 0); createRT(); return 0;
    }
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(h, m, w, l);
}

// Saves this window's own back buffer as a BMP. Only the GUI's own pixels - never the rest of the screen.
void saveBackBuffer(const char* path)
{
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(g_swap->GetBuffer(0, IID_PPV_ARGS(&bb))) || !bb) return;
    D3D11_TEXTURE2D_DESC d; bb->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (SUCCEEDED(g_dev->CreateTexture2D(&d, nullptr, &st))) {
        g_ctx->CopyResource(st, bb);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            FILE* f = fopen(path, "wb");
            if (f) {
                const uint32_t w = d.Width, h = d.Height, row = w * 4, size = 54 + row * h;
                uint8_t hdr[54] = { 'B', 'M' };
                auto put = [&](int o, uint32_t v) { hdr[o] = uint8_t(v); hdr[o + 1] = uint8_t(v >> 8); hdr[o + 2] = uint8_t(v >> 16); hdr[o + 3] = uint8_t(v >> 24); };
                put(2, size); put(10, 54); put(14, 40); put(18, w); put(22, h); hdr[26] = 1; hdr[28] = 32; put(34, row * h);
                fwrite(hdr, 1, 54, f);
                std::vector<uint8_t> line(row);
                for (int y = int(h) - 1; y >= 0; --y) {
                    const uint8_t* src = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
                    for (uint32_t x = 0; x < w; ++x) { line[4*x] = src[4*x+2]; line[4*x+1] = src[4*x+1]; line[4*x+2] = src[4*x]; line[4*x+3] = 255; }
                    fwrite(line.data(), 1, row, f);
                }
                fclose(f);
            }
            g_ctx->Unmap(st, 0);
        }
        st->Release();
    }
    bb->Release();
}
} // namespace

namespace GuiPlatform {

void beginRealtime()
{
    // An emulator is a real-time load: opt out of Windows' execution-speed throttling (EcoQoS),
    // which is applied to windows that are not in the foreground, and ask for 1 ms timer
    // granularity so the audio-paced 1 ms waits are 1 ms.
    PROCESS_POWER_THROTTLING_STATE pt{};
    pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    pt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    pt.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof(pt));
    timeBeginPeriod(1);
}
void endRealtime() { timeEndPeriod(1); }

bool createWindow(const char* title, int width, int height)
{
    g_wc = { sizeof(WNDCLASSEX), CS_CLASSDC, wndProc, 0, 0, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, _T("MS2000Thin"), nullptr };
    RegisterClassEx(&g_wc);
    g_hwnd = CreateWindowA("MS2000Thin", title, WS_OVERLAPPEDWINDOW, 30, 20, width, height, nullptr, nullptr, g_wc.hInstance, nullptr);
    if (!createDevice(g_hwnd)) { cleanupDevice(); UnregisterClass(g_wc.lpszClassName, g_wc.hInstance); return false; }
    createRT();
    ShowWindow(g_hwnd, SW_SHOWDEFAULT); UpdateWindow(g_hwnd);
    return true;
}

// VECTOR-PANEL legends: a narrow bold sans from Windows.
std::vector<std::string> panelFontFiles()
{
    return { "C:\\Windows\\Fonts\\ARIALNB.TTF", "C:\\Windows\\Fonts\\arialbd.ttf", "C:\\Windows\\Fonts\\segoeuib.ttf" };
}

void initBackends()
{
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);
}

bool pollEvents()
{
    MSG msg;
    bool alive = true;
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); if (msg.message == WM_QUIT) alive = false; }
    return alive;
}

void newFrame() { ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); }

void render(const float clear[4], const char* shotPath)
{
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_ctx->ClearRenderTargetView(g_rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    if (shotPath) saveBackBuffer(shotPath);
    g_swap->Present(1, 0);
}

void shutdownBackends() { ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); }

void destroyWindow()
{
    cleanupDevice();
    DestroyWindow(g_hwnd);
    UnregisterClass(g_wc.lpszClassName, g_wc.hInstance);
}

bool fileDialog(bool saving, std::string& out)
{
    char file[MAX_PATH] = {};
    OPENFILENAMEA ofn{}; ofn.lStructSize = sizeof ofn; ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = "SysEx (*.syx)\0*.syx\0All files\0*.*\0"; ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = "syx";
    ofn.Flags = OFN_NOCHANGEDIR | (saving ? OFN_OVERWRITEPROMPT : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST));
    if (!(saving ? GetSaveFileNameA(&ofn) : GetOpenFileNameA(&ofn))) return false;
    out = file; return true;
}

} // namespace GuiPlatform
