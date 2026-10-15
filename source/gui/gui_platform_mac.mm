// MAC-3: gui_platform.h on macOS - a GLFW window drawn with Metal (ImGui's glfw + metal backends), and the
// AppKit open/save panels. Compiled with ARC.
#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_metal.h"

#include "gui_platform.h"
#include <cstdio>
#include <cstdint>
#include <algorithm>
#include <vector>

namespace {
GLFWwindow*               g_win = nullptr;
id<MTLDevice>             g_device = nil;
id<MTLCommandQueue>       g_queue = nil;
CAMetalLayer*             g_layer = nil;
// The frame between newFrame() and render(): ImGui's Metal backend takes the render pass at NewFrame time.
MTLRenderPassDescriptor*  g_pass = nil;
id<CAMetalDrawable>       g_drawable = nil;

// BGRA8 pixels of a finished frame -> a bottom-up 32-bit BMP (the format the Windows build writes).
void saveBmp(const char* path, const uint8_t* bgra, uint32_t w, uint32_t h, size_t pitch)
{
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    const uint32_t row = w * 4, size = 54 + row * h;
    uint8_t hdr[54] = { 'B', 'M' };
    auto put = [&](int o, uint32_t v) { hdr[o] = uint8_t(v); hdr[o + 1] = uint8_t(v >> 8); hdr[o + 2] = uint8_t(v >> 16); hdr[o + 3] = uint8_t(v >> 24); };
    put(2, size); put(10, 54); put(14, 40); put(18, w); put(22, h); hdr[26] = 1; hdr[28] = 32; put(34, row * h);
    std::fwrite(hdr, 1, 54, f);
    for (int y = int(h) - 1; y >= 0; --y) {
        const uint8_t* src = bgra + size_t(y) * pitch;
        std::vector<uint8_t> line(src, src + row);
        for (uint32_t x = 0; x < w; ++x) line[4 * x + 3] = 255;
        std::fwrite(line.data(), 1, row, f);
    }
    std::fclose(f);
}
} // namespace

namespace GuiPlatform {

void beginRealtime() {}   // macOS: the emulation threads ask for QoS themselves (thread_prio_win.cpp)
void endRealtime() {}

bool createWindow(const char* title, int width, int height)
{
    if (!glfwInit()) return false;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    // Not larger than the screen it opens on (the Windows size is 1600 x 1060).
    if (GLFWmonitor* mon = glfwGetPrimaryMonitor()) {
        int x = 0, y = 0, w = 0, h = 0;
        glfwGetMonitorWorkarea(mon, &x, &y, &w, &h);
        if (w > 0 && h > 0) { width = std::min(width, w - 40); height = std::min(height, h - 40); }
    }
    g_win = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (!g_win) { glfwTerminate(); return false; }

    g_device = MTLCreateSystemDefaultDevice();
    if (!g_device) { glfwDestroyWindow(g_win); glfwTerminate(); return false; }
    g_queue = [g_device newCommandQueue];
    NSWindow* nsw = (NSWindow*)glfwGetCocoaWindow(g_win);
    g_layer = [CAMetalLayer layer];
    g_layer.device = g_device;
    g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    g_layer.framebufferOnly = NO;   // the MS2K_GUISHOT screenshot reads the drawable back
    nsw.contentView.layer = g_layer;
    nsw.contentView.wantsLayer = YES;
    return true;
}

std::vector<std::string> panelFontFiles()
{
    return { "/System/Library/Fonts/Supplemental/Arial Narrow Bold.ttf",
             "/Library/Fonts/Arial Narrow Bold.ttf",
             "/System/Library/Fonts/Supplemental/Arial Bold.ttf" };
}

void initBackends()
{
    ImGui_ImplGlfw_InitForOther(g_win, true);
    ImGui_ImplMetal_Init(g_device);
}

bool pollEvents()
{
    glfwPollEvents();
    return !glfwWindowShouldClose(g_win);
}

void newFrame()
{
    @autoreleasepool {
        int w = 0, h = 0;
        glfwGetFramebufferSize(g_win, &w, &h);
        g_layer.contentsScale = ((NSWindow*)glfwGetCocoaWindow(g_win)).backingScaleFactor;
        g_layer.drawableSize = CGSizeMake(std::max(w, 1), std::max(h, 1));
        g_drawable = [g_layer nextDrawable];
        g_pass = [MTLRenderPassDescriptor new];
        g_pass.colorAttachments[0].texture = g_drawable.texture;
        g_pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        g_pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        ImGui_ImplMetal_NewFrame(g_pass);
        ImGui_ImplGlfw_NewFrame();
    }
}

void render(const float clear[4], const char* shotPath)
{
    @autoreleasepool {
        if (!g_drawable) return;
        g_pass.colorAttachments[0].clearColor = MTLClearColorMake(clear[0], clear[1], clear[2], clear[3]);
        id<MTLCommandBuffer> cmd = [g_queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:g_pass];
        ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cmd, enc);
        [enc endEncoding];
        if (shotPath) {
            [cmd commit];
            [cmd waitUntilCompleted];
            id<MTLTexture> t = g_drawable.texture;
            const uint32_t w = uint32_t(t.width), h = uint32_t(t.height);
            std::vector<uint8_t> px(size_t(w) * h * 4);
            [t getBytes:px.data() bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
            saveBmp(shotPath, px.data(), w, h, size_t(w) * 4);
            [g_drawable present];
        } else {
            [cmd presentDrawable:g_drawable];
            [cmd commit];
        }
        g_drawable = nil;
        g_pass = nil;
    }
}

void shutdownBackends() { ImGui_ImplMetal_Shutdown(); ImGui_ImplGlfw_Shutdown(); }

void destroyWindow()
{
    g_layer = nil; g_queue = nil; g_device = nil;
    glfwDestroyWindow(g_win); g_win = nullptr;
    glfwTerminate();
}

bool fileDialog(bool saving, std::string& out)
{
    @autoreleasepool {
        UTType* syx = [UTType typeWithFilenameExtension:@"syx"];
        NSModalResponse r;
        NSURL* url = nil;
        if (saving) {
            NSSavePanel* p = [NSSavePanel savePanel];
            if (syx) p.allowedContentTypes = @[ syx ];
            p.nameFieldStringValue = @"ms2000_programs.syx";
            r = [p runModal];
            url = p.URL;
        } else {
            NSOpenPanel* p = [NSOpenPanel openPanel];
            p.canChooseFiles = YES; p.canChooseDirectories = NO; p.allowsMultipleSelection = NO;
            if (syx) p.allowedContentTypes = @[ syx ];
            r = [p runModal];
            url = p.URL;
        }
        if (r != NSModalResponseOK || !url) return false;
        out = url.fileSystemRepresentation;
        return true;
    }
}

bool chooseFolder(const char* message, std::string& out)
{
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp activateIgnoringOtherApps:YES];
        NSOpenPanel* p = [NSOpenPanel openPanel];
        p.canChooseFiles = NO; p.canChooseDirectories = YES; p.allowsMultipleSelection = NO;
        p.message = [NSString stringWithUTF8String:message];
        p.prompt = @"Choose";
        if ([p runModal] != NSModalResponseOK || !p.URL) return false;
        out = p.URL.fileSystemRepresentation;
        return true;
    }
}

void alert(const char* title, const char* text)
{
    @autoreleasepool {
        [NSApplication sharedApplication];
        NSAlert* a = [NSAlert new];
        a.messageText = [NSString stringWithUTF8String:title];
        a.informativeText = [NSString stringWithUTF8String:text];
        [a runModal];
    }
}

} // namespace GuiPlatform
