/*
 * wgl.c
 * Copyright (C) 2026 Kovid Goyal <kovid at kovidgoyal.net>
 * Based on wgl_context.c from GLFW, Copyright (c) 2002-2006 Marcus Geelnard
 * and (c) 2006-2019 Camilla Löwy
 *
 * Distributed under terms of the GPL3 license.
 */

// OpenGL contexts for Win32 windows, created using WGL.

#include "platform.h"
#include "version.h"
#include <windows.h>
#include <stdlib.h>
#include <string.h>

#define WGL_DRAW_TO_WINDOW_ARB 0x2001
#define WGL_ACCELERATION_ARB 0x2003
#define WGL_SUPPORT_OPENGL_ARB 0x2010
#define WGL_DOUBLE_BUFFER_ARB 0x2011
#define WGL_PIXEL_TYPE_ARB 0x2013
#define WGL_COLOR_BITS_ARB 0x2014
#define WGL_ALPHA_BITS_ARB 0x201b
#define WGL_FULL_ACCELERATION_ARB 0x2027
#define WGL_TYPE_RGBA_ARB 0x202b
#define WGL_FRAMEBUFFER_SRGB_CAPABLE_ARB 0x20a9
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_FLAGS_ARB 0x2094
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB 0x00000002
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001

static struct {
    bool initialized, ARB_pixel_format, ARB_framebuffer_sRGB, EXT_swap_control, ARB_create_context;
    HMODULE opengl32;
    BOOL(WINAPI *SwapIntervalEXT)(int);
    BOOL(WINAPI *ChoosePixelFormatARB)(HDC, const int *, const FLOAT *, UINT, int *, UINT *);
    const char *(WINAPI *GetExtensionsStringARB)(HDC);
    const char *(WINAPI *GetExtensionsStringEXT)(void);
    HGLRC(WINAPI *CreateContextAttribsARB)(HDC, HGLRC, const int *);
} wgl = {0};

struct GLSurface {
    HDC dc;
    HGLRC context;
    int interval;
    GPUWin32Presenter *presenter;
    GLSurface *next;
};

static const GLPlatformHost *host = NULL;
static GLSurface *all_surfaces = NULL;

static gl_proc
get_proc_address(const char *name) {
    PROC p = wglGetProcAddress(name);
    // Some drivers return small integers instead of NULL for failure
    const intptr_t v = (intptr_t)p;
    // The OpenGL 1.1 entry points are exported by opengl32.dll itself, not by the driver
    if (v >= -1 && v <= 3) p = GetProcAddress(wgl.opengl32, name);
    gl_proc ans;
    memcpy(&ans, &p, sizeof(ans));
    return ans;
}

#define LOAD_EXT(dest, name)                 \
    do {                                     \
        gl_proc p_ = get_proc_address(name); \
        memcpy(&(dest), &p_, sizeof(p_));    \
    } while (0)

static bool
extension_supported(HDC dc, const char *name) {
    const char *extensions = NULL;
    if (wgl.GetExtensionsStringARB) extensions = wgl.GetExtensionsStringARB(dc);
    else if (wgl.GetExtensionsStringEXT) extensions = wgl.GetExtensionsStringEXT();
    return extensions && gl_extension_in_string(name, extensions);
}

// The extension entry points can only be loaded with a context current, and a
// pixel format can be set only once per window, so use a legacy context on a
// throwaway window to load them.
static bool
initialize(void) {
    if (wgl.initialized) return true;
    wgl.opengl32 = GetModuleHandleW(L"opengl32.dll");
    if (!wgl.opengl32) {
        host->set_error("WGL: opengl32.dll is not loaded");
        return false;
    }
    HWND probe = CreateWindowExW(0, L"STATIC", L"kitty-wgl-probe", WS_POPUP, 0, 0, 1, 1, NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!probe) {
        host->set_error("WGL: Failed to create a window to load extensions with error: %lu", GetLastError());
        return false;
    }
    HDC dc = GetDC(probe);
    PIXELFORMATDESCRIPTOR pfd = {
        .nSize = sizeof(pfd),
        .nVersion = 1,
        .dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
        .iPixelType = PFD_TYPE_RGBA,
        .cColorBits = 24,
    };
    HGLRC rc = NULL;
    const HDC previous_dc = wglGetCurrentDC();
    const HGLRC previous_context = wglGetCurrentContext();
    if (!SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd) || !(rc = wglCreateContext(dc)) || !wglMakeCurrent(dc, rc)) {
        host->set_error("WGL: Failed to create a legacy context to load extensions with error: %lu", GetLastError());
        if (rc) wglDeleteContext(rc);
        ReleaseDC(probe, dc);
        DestroyWindow(probe);
        return false;
    }
    LOAD_EXT(wgl.GetExtensionsStringARB, "wglGetExtensionsStringARB");
    LOAD_EXT(wgl.GetExtensionsStringEXT, "wglGetExtensionsStringEXT");
    if (extension_supported(dc, "WGL_EXT_swap_control")) {
        LOAD_EXT(wgl.SwapIntervalEXT, "wglSwapIntervalEXT");
        wgl.EXT_swap_control = wgl.SwapIntervalEXT != NULL;
    }
    if (extension_supported(dc, "WGL_ARB_pixel_format")) {
        LOAD_EXT(wgl.ChoosePixelFormatARB, "wglChoosePixelFormatARB");
        wgl.ARB_pixel_format = wgl.ChoosePixelFormatARB != NULL;
    }
    if (extension_supported(dc, "WGL_ARB_create_context")) {
        LOAD_EXT(wgl.CreateContextAttribsARB, "wglCreateContextAttribsARB");
        wgl.ARB_create_context = wgl.CreateContextAttribsARB != NULL;
    }
    wgl.ARB_framebuffer_sRGB = extension_supported(dc, "WGL_ARB_framebuffer_sRGB") || extension_supported(dc, "WGL_EXT_framebuffer_sRGB");
    wglMakeCurrent(previous_dc, previous_context);
    wglDeleteContext(rc);
    ReleaseDC(probe, dc);
    DestroyWindow(probe);
    wgl.initialized = true;
    return true;
}
#undef LOAD_EXT

static int
choose_pixel_format(HDC dc, const GPUSurfaceConfig *config) {
    const int alpha_bits = config->transparent ? 8 : 0;
    if (wgl.ARB_pixel_format) {
        for (int srgb = config->srgb && wgl.ARB_framebuffer_sRGB; srgb >= 0; srgb--) {
            int attribs[] = {
                WGL_DRAW_TO_WINDOW_ARB,
                TRUE,
                WGL_SUPPORT_OPENGL_ARB,
                TRUE,
                WGL_DOUBLE_BUFFER_ARB,
                TRUE,
                WGL_PIXEL_TYPE_ARB,
                WGL_TYPE_RGBA_ARB,
                WGL_ACCELERATION_ARB,
                WGL_FULL_ACCELERATION_ARB,
                WGL_COLOR_BITS_ARB,
                24,
                WGL_ALPHA_BITS_ARB,
                alpha_bits,
                srgb ? WGL_FRAMEBUFFER_SRGB_CAPABLE_ARB : 0,
                TRUE,
                0,
                0,
            };
            int format = 0;
            UINT count = 0;
            if (wgl.ChoosePixelFormatARB(dc, attribs, NULL, 1, &format, &count) && count) return format;
        }
    }
    PIXELFORMATDESCRIPTOR pfd = {
        .nSize = sizeof(pfd),
        .nVersion = 1,
        .dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
        .iPixelType = PFD_TYPE_RGBA,
        .cColorBits = 24,
        .cAlphaBits = (BYTE)alpha_bits,
    };
    return ChoosePixelFormat(dc, &pfd);
}

static bool
choose_native_visual(const GPUNativeWindow *native, const GPUSurfaceConfig *config, uint64_t *visual_id) {
    // The pixel format is chosen per window, in surface_create()
    (void)native;
    (void)config;
    *visual_id = 0;
    return true;
}

static bool
make_current(GLSurface *s) {
    if (!wglMakeCurrent(s ? s->dc : NULL, s ? s->context : NULL)) {
        host->set_error("WGL: Failed to %s context with error: %lu", s ? "make current" : "clear current", GetLastError());
        return false;
    }
    return true;
}

static void
surface_destroy(GLSurface *s) {
    if (!s) return;
    if (s->context) {
        // The presenter's OpenGL objects belong to this context, make it
        // current to release them, then restore whichever context was current
        const HDC previous_dc = wglGetCurrentDC();
        const HGLRC previous_context = wglGetCurrentContext();
        if (s->presenter && wglMakeCurrent(s->dc, s->context)) {
            s->presenter->release(s->presenter);
            if (previous_context == s->context) wglMakeCurrent(NULL, NULL);
            else wglMakeCurrent(previous_dc, previous_context);
        }
        wglDeleteContext(s->context);
    }
    // The DC belongs to the window, whose class has CS_OWNDC, so is not released
    for (GLSurface **p = &all_surfaces; *p; p = &(*p)->next) {
        if (*p == s) {
            *p = s->next;
            break;
        }
    }
    free(s);
}

static GLSurface *
surface_create(const GPUNativeWindow *native, const GPUSurfaceConfig *config, uint32_t width, uint32_t height) {
    (void)width;
    (void)height;
    if (native->platform != GPU_PLATFORM_WIN32 || !native->handle) {
        host->set_error("WGL: Only Win32 windows are supported");
        return NULL;
    }
    if (!initialize()) return NULL;
    HDC dc = GetDC((HWND)native->handle);
    if (!dc) {
        host->set_error("WGL: Failed to get the device context of the window");
        return NULL;
    }
    const int format = choose_pixel_format(dc, config);
    PIXELFORMATDESCRIPTOR pfd;
    if (!format || !DescribePixelFormat(dc, format, sizeof(pfd), &pfd) || !SetPixelFormat(dc, format, &pfd)) {
        host->set_error("WGL: Failed to set a pixel format for the window with error: %lu", GetLastError());
        return NULL;
    }
    GLSurface *s = calloc(1, sizeof(GLSurface));
    if (!s) {
        host->set_error("WGL: Out of memory");
        return NULL;
    }
    s->dc = dc;
    s->presenter = native->win32_presenter;
    s->interval = config->swap_interval;
    s->next = all_surfaces;
    all_surfaces = s;
    // Share objects with any existing context
    HGLRC share = s->next ? s->next->context : NULL;
    if (wgl.ARB_create_context) {
        const int attribs[] = {
            WGL_CONTEXT_MAJOR_VERSION_ARB,
            OPENGL_REQUIRED_VERSION_MAJOR,
            WGL_CONTEXT_MINOR_VERSION_ARB,
            OPENGL_REQUIRED_VERSION_MINOR,
            WGL_CONTEXT_FLAGS_ARB,
            WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB,
            WGL_CONTEXT_PROFILE_MASK_ARB,
            WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
            0,
            0,
        };
        s->context = wgl.CreateContextAttribsARB(dc, share, attribs);
    } else if ((s->context = wglCreateContext(dc)) && share && !wglShareLists(share, s->context)) {
        wglDeleteContext(s->context);
        s->context = NULL;
    }
    if (!s->context) {
        host->set_error(
            "WGL: Failed to create context with error: %lu. kitty requires working OpenGL %d.%d drivers.",
            GetLastError(),
            OPENGL_REQUIRED_VERSION_MAJOR,
            OPENGL_REQUIRED_VERSION_MINOR);
        surface_destroy(s);
        return NULL;
    }
    return s;
}

static void
surface_resize(GLSurface *s, uint32_t width, uint32_t height) {
    // The window framebuffer is resized along with the window, a composition
    // swapchain, if any, by the windowing system
    (void)s;
    (void)width;
    (void)height;
}

static bool
present(GLSurface *s) {
    if (s->presenter && s->presenter->present(s->presenter, s->interval)) return true;
    if (!SwapBuffers(s->dc)) {
        host->set_error("WGL: SwapBuffers failed with error: %lu", GetLastError());
        return false;
    }
    return true;
}

static void
set_swap_interval(GLSurface *s, int interval) {
    s->interval = interval;
    // With a presenter, pacing is done by its present(), see GPUWin32Presenter
    if (wgl.EXT_swap_control) wgl.SwapIntervalEXT(s->presenter ? 0 : interval);
}

static void
native_changed(GLSurface *s) {
    (void)s;
}

static bool
recreate(GLSurface *s) {
    (void)s;
    host->set_error("WGL: Recreating surfaces is not supported");
    return false;
}

static void
terminate(void) {
    while (all_surfaces) surface_destroy(all_surfaces);
    wgl.initialized = false;
}

const GLPlatform *
wgl_platform(const GLPlatformHost *h) {
    static const GLPlatform ans = {
        .name = "WGL",
        .choose_native_visual = choose_native_visual,
        .surface_create = surface_create,
        .surface_destroy = surface_destroy,
        .make_current = make_current,
        .surface_resize = surface_resize,
        .present = present,
        .set_swap_interval = set_swap_interval,
        .native_changed = native_changed,
        .recreate = recreate,
        .get_proc_address = get_proc_address,
        .terminate = terminate,
    };
    host = h;
    return &ans;
}
