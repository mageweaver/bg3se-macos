/**
 * @file imgui_metal_backend.mm
 * @brief Metal rendering backend for ImGui overlay on macOS BG3
 *
 * Implementation strategy:
 * 1. Swizzle CAMetalLayer's nextDrawable to detect when game is about to render
 * 2. After game renders, inject ImGui frame into the same command buffer
 * 3. Use ImGui's official Metal backend for actual rendering
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <objc/runtime.h>
#import <mach/mach_time.h>

#include "imgui_metal_backend.h"
#include "imgui_input_hooks.h"
#include "imgui_objects.h"
#include "lua_imgui.h"
#include "imgui.h"
#include "imgui_impl_metal.h"

#include <pthread.h>
#include <unistd.h>
#include "imgui_impl_osx.h"
#include "logging.h"

// ============================================================================
// Static State
// ============================================================================

static struct {
    ImguiMetalState state;
    bool visible;
    bool pending_visible;      // Visibility requested before ready
    bool capturing_input;

    // Metal state
    id<MTLDevice> device;
    id<MTLCommandQueue> commandQueue;
    MTLRenderPassDescriptor *renderPassDescriptor;

    // Original method implementations
    IMP original_nextDrawable;
    IMP original_present;

    // Current drawable for rendering (set by nextDrawable, used by present)
    id<CAMetalDrawable> currentDrawable;

    // Target layer (cached after first detection)
    CAMetalLayer *gameLayer;
    NSWindow *gameWindow;

    // Frame tracking
    uint64_t frame_count;
    bool needs_font_rebuild;

    // Ext.IMGUI parity surface (ClientIMGUI.inl)
    bool demo_enabled;
    float ui_scale_multiplier;
    float font_scale_multiplier;
    bool style_baseline_captured;
} s_state = {
    IMGUI_METAL_STATE_UNINITIALIZED,  // state
    false,                              // visible
    false,                              // pending_visible
    false,                              // capturing_input
    nil,                                // device
    nil,                                // commandQueue
    nil,                                // renderPassDescriptor
    NULL,                               // original_nextDrawable
    NULL,                               // original_present
    nil,                                // currentDrawable
    nil,                                // gameLayer
    nil,                                // gameWindow
    0,                                  // frame_count
    false,                              // needs_font_rebuild
    false,                              // demo_enabled
    1.0f,                               // ui_scale_multiplier
    1.0f,                               // font_scale_multiplier
    false                               // style_baseline_captured
};

// ============================================================================
// Ext.IMGUI parity surface: demo window, scaling, custom fonts
// ============================================================================
//
// Windows reference: BG3Extender/Lua/Libs/ClientIMGUI.inl:20-49.
//
// Font loading cannot happen on the Lua thread: the atlas is owned by the
// render thread and rebuilding it mid-frame invalidates the Metal font
// texture. Requests are therefore queued here and applied at frame start,
// which is also when the scale multipliers are re-derived.

#define IMGUI_MAX_PENDING_FONTS 16
#define IMGUI_FONT_NAME_LEN     64
#define IMGUI_FONT_PATH_LEN     1024

typedef struct {
    char name[IMGUI_FONT_NAME_LEN];
    char path[IMGUI_FONT_PATH_LEN];
    float size;
} PendingFont;

static PendingFont s_pending_fonts[IMGUI_MAX_PENDING_FONTS];
static int s_pending_font_count = 0;
static pthread_mutex_t s_font_mutex = PTHREAD_MUTEX_INITIALIZER;

// Named fonts that have been successfully loaded, so Ext.IMGUI can look them
// up later and so a duplicate LoadFont() is reported honestly.
typedef struct {
    char name[IMGUI_FONT_NAME_LEN];
    ImFont *font;
} LoadedFont;

static LoadedFont s_loaded_fonts[IMGUI_MAX_PENDING_FONTS];
static int s_loaded_font_count = 0;

// Pristine style captured before any scaling, so SetUIScaleMultiplier is
// idempotent rather than cumulative (ScaleAllSizes multiplies in place).
static ImGuiStyle s_style_baseline;

void imgui_metal_set_demo_enabled(bool enabled) {
    s_state.demo_enabled = enabled;
    LOG_IMGUI_INFO("ImGui demo window %s", enabled ? "enabled" : "disabled");
}

bool imgui_metal_get_demo_enabled(void) {
    return s_state.demo_enabled;
}

void imgui_metal_set_ui_scale_multiplier(float scale) {
    if (!(scale > 0.0f) || scale > 10.0f) {
        LOG_IMGUI_WARN("SetUIScaleMultiplier: ignoring out-of-range scale %f", scale);
        return;
    }
    s_state.ui_scale_multiplier = scale;
    s_state.needs_font_rebuild = true;  // re-derive style at frame start
}

float imgui_metal_get_ui_scale_multiplier(void) {
    return s_state.ui_scale_multiplier;
}

void imgui_metal_set_font_scale_multiplier(float scale) {
    if (!(scale > 0.0f) || scale > 10.0f) {
        LOG_IMGUI_WARN("SetFontScaleMultiplier: ignoring out-of-range scale %f", scale);
        return;
    }
    s_state.font_scale_multiplier = scale;
}

float imgui_metal_get_font_scale_multiplier(void) {
    return s_state.font_scale_multiplier;
}

bool imgui_metal_load_font(const char *name, const char *path, float size) {
    if (!name || !path || !(size > 0.0f)) {
        return false;
    }
    if (access(path, R_OK) != 0) {
        LOG_IMGUI_ERROR("LoadFont('%s'): cannot read '%s'", name, path);
        return false;
    }

    pthread_mutex_lock(&s_font_mutex);
    if (s_pending_font_count >= IMGUI_MAX_PENDING_FONTS) {
        pthread_mutex_unlock(&s_font_mutex);
        LOG_IMGUI_ERROR("LoadFont('%s'): pending font queue is full", name);
        return false;
    }

    PendingFont *slot = &s_pending_fonts[s_pending_font_count++];
    snprintf(slot->name, sizeof(slot->name), "%s", name);
    snprintf(slot->path, sizeof(slot->path), "%s", path);
    slot->size = size;
    pthread_mutex_unlock(&s_font_mutex);

    s_state.needs_font_rebuild = true;
    LOG_IMGUI_INFO("LoadFont('%s') queued: %s @ %.1fpx", name, path, size);
    return true;
}

// Apply queued font loads and scale changes. Render thread only, called before
// ImGui::NewFrame() so the atlas is not mutated mid-frame.
static void imgui_metal_apply_pending_style(void) {
    ImGuiIO &io = ImGui::GetIO();

    if (!s_state.style_baseline_captured) {
        s_style_baseline = ImGui::GetStyle();
        s_state.style_baseline_captured = true;
    }

    // Font scale is a plain per-frame multiplier; no atlas work required.
    io.FontGlobalScale = s_state.font_scale_multiplier;

    if (!s_state.needs_font_rebuild) {
        return;
    }
    s_state.needs_font_rebuild = false;

    // Re-derive the style from the pristine baseline so repeated calls do not
    // compound.
    ImGuiStyle &style = ImGui::GetStyle();
    style = s_style_baseline;
    if (s_state.ui_scale_multiplier != 1.0f) {
        style.ScaleAllSizes(s_state.ui_scale_multiplier);
    }

    // Drain the pending font queue.
    pthread_mutex_lock(&s_font_mutex);
    int count = s_pending_font_count;
    PendingFont batch[IMGUI_MAX_PENDING_FONTS];
    if (count > 0) {
        memcpy(batch, s_pending_fonts, sizeof(PendingFont) * (size_t)count);
        s_pending_font_count = 0;
    }
    pthread_mutex_unlock(&s_font_mutex);

    if (count == 0) {
        return;
    }

    bool added = false;
    for (int i = 0; i < count; i++) {
        ImFont *font = io.Fonts->AddFontFromFileTTF(batch[i].path, batch[i].size);
        if (!font) {
            LOG_IMGUI_ERROR("LoadFont('%s'): ImGui rejected %s", batch[i].name,
                            batch[i].path);
            continue;
        }
        added = true;

        if (s_loaded_font_count < IMGUI_MAX_PENDING_FONTS) {
            LoadedFont *slot = &s_loaded_fonts[s_loaded_font_count++];
            snprintf(slot->name, sizeof(slot->name), "%s", batch[i].name);
            slot->font = font;
        }
        LOG_IMGUI_INFO("LoadFont('%s'): loaded", batch[i].name);
    }

    if (added) {
        // ImGui 1.92+ owns the atlas texture lifecycle: fonts added at runtime
        // are uploaded by the Metal backend during RenderDrawData via the
        // ImTextureData path, so no manual destroy/create is needed (and the
        // old ImGui_ImplMetal_*FontsTexture entry points no longer exist).
        LOG_IMGUI_INFO("Queued %d font(s) into the ImGui atlas", count);
    }
}

// ============================================================================
// CGEventTap Mouse Position Cache
// ============================================================================
// We store the last known mouse position from CGEventTap because
// ImGui_ImplOSX_NewFrame() overwrites io.MousePos with Cocoa's mouse location,
// which doesn't work for SDL games. After OSX backend runs, we re-apply our position.
static struct {
    float x, y;
    bool valid;
} s_cgevent_mouse = {0, 0, false};

static bool s_first_drawable_seen = false;

// ============================================================================
// Forward Declarations
// ============================================================================

static void imgui_metal_setup_context(void);
static void imgui_metal_render_frame(id<CAMetalDrawable> drawable,
                                     id<MTLCommandBuffer> gameCommandBuffer);
static id<CAMetalDrawable> hooked_nextDrawable(id self, SEL _cmd);
static void hooked_present(id self, SEL _cmd);
static void install_command_buffer_hook(void);
static void hooked_commit(id self, SEL _cmd);

// ============================================================================
// Present Hook State (declared early for use in remove_layer_hook)
// ============================================================================

static Class s_drawableClass = nil;
static bool s_presentHooked = false;

// Preferred render path: encode ImGui into the game's own command buffer from
// -[MTLCommandBuffer presentDrawable:], which the engine calls on its render
// thread. While this is armed the -[CAMetalDrawable present] hook renders
// nothing, so the overlay is drawn exactly once per frame.
static IMP s_original_presentDrawable = NULL;
static IMP s_original_presentDrawableOptions = NULL;
static IMP s_original_commit = NULL;
static Class s_commitClass = nil;
static bool s_cmdBufHooked = false;
static bool s_commitHooked = false;

// Swizzle a selector only where the class implements it ITSELF.
//
// class_getInstanceMethod walks up to the first superclass that implements the
// selector, and patching that Method replaces the SUPERCLASS implementation.
// For -commit that put our hook underneath AGXG16XFamilyCommandBuffer's own
// commit (which calls super), so we ran after AGX had already started
// committing the buffer and crashed binding a render target it had let go.
static bool swizzle_own_method(Class cls, SEL sel, IMP newImp, IMP *outOriginal) {
    unsigned int count = 0;
    Method *methods = class_copyMethodList(cls, &count);
    if (!methods) return false;

    Method own = NULL;
    for (unsigned int i = 0; i < count; i++) {
        if (method_getName(methods[i]) == sel) {
            own = methods[i];
            break;
        }
    }
    free(methods);
    if (!own) return false;

    if (outOriginal) *outOriginal = method_getImplementation(own);
    method_setImplementation(own, newImp);
    return true;
}

// presentDrawable: records which drawable a command buffer will present; the
// encode happens later, in commit. Several buffers can be in flight, so keep a
// small table rather than a single slot.
#define IMGUI_PENDING_PRESENTS 8
static struct {
    __unsafe_unretained id commandBuffer;   // identity only, never messaged
    __unsafe_unretained id<CAMetalDrawable> drawable;
} s_pendingPresents[IMGUI_PENDING_PRESENTS];
static pthread_mutex_t s_pendingMutex = PTHREAD_MUTEX_INITIALIZER;

// Why a frame did or did not reach the screen. Sampled into the log so a single
// session answers it, instead of one launch per hypothesis.
static struct {
    uint64_t commits;       // hooked_commit entered
    uint64_t recorded;      // presentDrawable: seen
    uint64_t table_full;    // present dropped, no free slot
    uint64_t matched;       // commit found this buffer's drawable
    uint64_t skip_state;    // backend not ready / nothing visible
    uint64_t skip_layer;    // drawable belongs to a foreign layer
    uint64_t skip_texture;  // drawable texture already gone
    uint64_t rendered;
} s_diag;

static bool imgui_metal_has_visible_window(void);

// Records why the answer was "no", so a dark overlay names its own cause
// instead of costing a launch per hypothesis.
static struct {
    uint64_t lock_fail;     // pool lock was held; we never even looked
    uint64_t no_windows;    // pool has no window objects
    uint64_t all_hidden;    // windows exist but none reports visible
    int last_window_count;
    int last_visible;       // flags of window 0 the last time we looked
    int last_open;
} s_visDiag;

static void imgui_metal_log_diag(void) {
    LOG_IMGUI_DEBUG("overlay: commits=%llu presents=%llu matched=%llu rendered=%llu "
                   "| skipped state=%llu layer=%llu texture=%llu | table_full=%llu",
                   s_diag.commits, s_diag.recorded, s_diag.matched, s_diag.rendered,
                   s_diag.skip_state, s_diag.skip_layer, s_diag.skip_texture,
                   s_diag.table_full);
    LOG_IMGUI_DEBUG("overlay: visible=%d (backend_visible=%d state=%d) "
                   "| no: lock=%llu none=%llu hidden=%llu "
                   "| windows=%d window0 visible=%d open=%d",
                   imgui_metal_has_visible_window() ? 1 : 0,
                   s_state.visible ? 1 : 0, (int)s_state.state,
                   s_visDiag.lock_fail, s_visDiag.no_windows, s_visDiag.all_hidden,
                   s_visDiag.last_window_count, s_visDiag.last_visible,
                   s_visDiag.last_open);
}

static void imgui_metal_record_present(id commandBuffer, id<CAMetalDrawable> drawable) {
    pthread_mutex_lock(&s_pendingMutex);
    s_diag.recorded++;
    bool stored = false;
    for (int i = 0; i < IMGUI_PENDING_PRESENTS; i++) {
        if (s_pendingPresents[i].commandBuffer == nil ||
            s_pendingPresents[i].commandBuffer == commandBuffer) {
            s_pendingPresents[i].commandBuffer = commandBuffer;
            s_pendingPresents[i].drawable = drawable;
            stored = true;
            break;
        }
    }
    if (!stored) {
        // Every slot holds a buffer that never came back through commit. Drop
        // the oldest rather than going permanently deaf.
        s_diag.table_full++;
        s_pendingPresents[0].commandBuffer = commandBuffer;
        s_pendingPresents[0].drawable = drawable;
    }
    pthread_mutex_unlock(&s_pendingMutex);
}

static id<CAMetalDrawable> imgui_metal_take_present(id commandBuffer) {
    id<CAMetalDrawable> drawable = nil;
    pthread_mutex_lock(&s_pendingMutex);
    for (int i = 0; i < IMGUI_PENDING_PRESENTS; i++) {
        if (s_pendingPresents[i].commandBuffer == commandBuffer) {
            drawable = s_pendingPresents[i].drawable;
            s_pendingPresents[i].commandBuffer = nil;
            s_pendingPresents[i].drawable = nil;
            break;
        }
    }
    pthread_mutex_unlock(&s_pendingMutex);
    return drawable;
}

// ============================================================================
// Method Swizzling
// ============================================================================

static void install_layer_hook(void) {
    // Swizzle CAMetalLayer's nextDrawable method
    Method original = class_getInstanceMethod([CAMetalLayer class], @selector(nextDrawable));
    if (!original) {
        LOG_IMGUI_ERROR("Could not find CAMetalLayer nextDrawable method");
        s_state.state = IMGUI_METAL_STATE_ERROR;
        return;
    }

    s_state.original_nextDrawable = method_getImplementation(original);
    method_setImplementation(original, (IMP)hooked_nextDrawable);

    LOG_IMGUI_INFO("Installed CAMetalLayer hook, waiting for game render...");
    s_state.state = IMGUI_METAL_STATE_WAITING_FOR_DEVICE;
}

static void remove_layer_hook(void) {
    // Restore nextDrawable
    if (s_state.original_nextDrawable) {
        Method original = class_getInstanceMethod([CAMetalLayer class], @selector(nextDrawable));
        if (original) {
            method_setImplementation(original, s_state.original_nextDrawable);
        }
        s_state.original_nextDrawable = NULL;
    }

    // Restore present on the drawable class
    if (s_state.original_present && s_drawableClass) {
        Method presentMethod = class_getInstanceMethod(s_drawableClass, @selector(present));
        if (presentMethod) {
            method_setImplementation(presentMethod, s_state.original_present);
        }
        s_state.original_present = NULL;
        s_presentHooked = false;
        s_drawableClass = nil;
    }
}

// ============================================================================
// Visibility / Input Predicates
//
// Mod-created windows (e.g. MCM) set window.Visible = true (obj->styled.visible)
// but do NOT toggle the F11 debug overlay (s_state.visible). To make those
// windows render and be interactive on their own, we OR the overlay state with
// "is any mod window visible" at the render and input gates. The F11 overlay
// contract is preserved exactly: when no mod window is up, every gate collapses
// back to the original overlay-only condition.
// ============================================================================

// True if any Lua/mod-created window is currently visible.
// The pool lock (recursive) must be held across the whole iteration:
// imgui_get_all_windows returns the pool's realloc-able array, and the main
// thread growing it while the render/input thread iterates is a use-after-
// free. Trylock keeps the render and input threads non-blocking — on
// contention we report "no window" for one poll, which only delays input
// capture by a frame.
static bool imgui_metal_has_visible_window(void) {
    if (!imgui_objects_trylock()) {
        s_visDiag.lock_fail++;
        return false;
    }
    bool found = false;
    int n = 0;
    ImguiHandle *w = imgui_get_all_windows(&n);
    s_visDiag.last_window_count = n;
    for (int i = 0; w && i < n; i++) {
        ImguiObject *o = imgui_object_get(w[i]);
        if (!o || o->type != IMGUI_OBJ_WINDOW) continue;
        if (i == 0) {
            s_visDiag.last_visible = o->styled.visible ? 1 : 0;
            s_visDiag.last_open = o->data.window.open ? 1 : 0;
        }

        // Report every open/visible transition regardless of who caused it.
        // The Lua .Open/.Visible setters log their own changes, but the ImGui
        // X-button path in render_window flips styled.visible directly and was
        // invisible in the log. This walk runs every frame, so it catches both.
        if (i < 4) {
            static struct { ImguiHandle h; int vis; int open; bool init; } s_last[4];
            int vis = o->styled.visible ? 1 : 0;
            int open = o->data.window.open ? 1 : 0;
            if (!s_last[i].init || s_last[i].h != w[i] ||
                s_last[i].vis != vis || s_last[i].open != open) {
                if (s_last[i].init && s_last[i].h == w[i]) {
                    LOG_IMGUI_INFO("window '%s' state: visible %d->%d open %d->%d",
                                   o->styled.label, s_last[i].vis, vis,
                                   s_last[i].open, open);
                }
                s_last[i].h = w[i];
                s_last[i].vis = vis;
                s_last[i].open = open;
                s_last[i].init = true;
            }
        }
        if (o->styled.visible) {
            found = true;
            break;
        }
    }
    imgui_objects_unlock();

    if (!found) {
        if (n == 0) s_visDiag.no_windows++;
        else s_visDiag.all_hidden++;
    }
    return found;
}

// Feed gate: ImGui has something on screen that should receive input — the F11
// overlay or any visible mod window. Requires the backend to be READY.
// (Replaces the old `!s_state.visible || state != READY` early-return.)
static bool imgui_input_target_present(void) {
    return s_state.state == IMGUI_METAL_STATE_READY &&
           (s_state.visible || imgui_metal_has_visible_window());
}

// Capture gate: ImGui is allowed to CONSUME the input it wants. True when the
// overlay is in capture mode OR a mod window is visible. Consumption is still
// ultimately governed by io.WantCapture* at the call site, so game input passes
// through whenever the cursor/focus is not over an ImGui widget. When neither
// the overlay is capturing nor a mod window is up, this is false and nothing is
// ever taken from the game.
static bool imgui_input_capture_active(void) {
    return s_state.capturing_input || imgui_metal_has_visible_window();
}

// ============================================================================
// Drawable Present Hook
// ============================================================================

// We hook the present method on CAMetalDrawable's concrete class.
// This allows us to render ImGui AFTER the game finishes but BEFORE the frame is shown.

// True when this drawable belongs to the layer the game renders its main scene
// into. During cutscenes/movies and other transitions the game presents
// drawables from a different CAMetalLayer; rendering our ImGui pass
// (sized/formatted for the main layer) into those causes a GPU fault.
// The layer the game presents through is not fixed for the life of the process.
// s_state.gameLayer is captured on the first drawable we ever see, and when a
// mod window brings the backend up during the intro movie that is the MOVIE's
// layer. Latching it meant every post-movie drawable looked foreign and the
// overlay silently stopped rendering for the rest of the session.
//
// So treat the cached layer as a fast path only, and on a mismatch re-resolve
// by asking whether any app window is presenting through this layer. If one is,
// that is the game layer now.
static bool imgui_metal_drawable_is_game_layer(id<CAMetalDrawable> drawable) {
    CAMetalLayer *layer = drawable.layer;
    if (!layer) return false;

    if (s_state.gameLayer != nil && layer == s_state.gameLayer) return true;

    for (NSWindow *window in [NSApp windows]) {
        NSView *contentView = [window contentView];
        if (contentView && (CALayer *)layer == [contentView layer]) {
            if (s_state.gameLayer != layer) {
                LOG_IMGUI_INFO("Game layer changed (window '%s'); overlay follows it",
                               [[window title] UTF8String]);
            }
            s_state.gameLayer = layer;
            s_state.gameWindow = window;
            return true;
        }
    }

    return false;
}

static bool imgui_metal_should_render(void) {
    return s_state.state == IMGUI_METAL_STATE_READY &&
           (s_state.visible || imgui_metal_has_visible_window());
}

// The overlay is encoded from -[MTLCommandBuffer commit].
//
// commit runs on whichever thread submitted the frame - the engine's render
// thread - and Metal requires every encoder to be ended before it, so this is
// the only point where we can legally open our own encoder on the game's
// buffer. presentDrawable: is too early: the engine still has an encoder open
// there, and AGX aborts the process with
//   "A command encoder is already encoding to this command buffer".
// So presentDrawable: only records which drawable this buffer will present.
//
// Encoding into the game's buffer (rather than our own queue) is what puts the
// overlay on top: our pass is the last one in the buffer, and the drawable is
// presented only once that whole buffer completes. A separate queue cannot get
// that ordering without an explicit GPU wait.
//
// The -[CAMetalDrawable present] hook below cannot do this: Metal defers that
// call into the command buffer's scheduled handler, so it fires on
// com.Metal.CompletionQueueDispatch while the render thread is already building
// the next frame. Running ImGui and a second command queue there raced the
// engine's own encoding and segfaulted it inside
// renderCommandEncoderWithDescriptor:.
// Arm -commit on the concrete command buffer class the engine actually uses.
// Only reachable from a live buffer, which is why it is done here rather than
// in install_command_buffer_hook.
static void arm_commit_hook_for(id commandBuffer) {
    if (s_commitHooked) return;

    for (Class cls = object_getClass(commandBuffer); cls; cls = class_getSuperclass(cls)) {
        if (swizzle_own_method(cls, @selector(commit), (IMP)hooked_commit,
                               &s_original_commit)) {
            s_commitClass = cls;
            s_commitHooked = true;
            LOG_IMGUI_INFO("Hooked -commit on %s (overlay encodes here)",
                           class_getName(cls));
            return;
        }
    }

    LOG_IMGUI_WARN("No -commit implementation found on the command buffer class "
                   "chain; overlay will not render");
}

static void hooked_presentDrawable(id self, SEL _cmd, id<CAMetalDrawable> drawable) {
    if (drawable) {
        arm_commit_hook_for(self);
        imgui_metal_record_present(self, drawable);
    }
    if (s_original_presentDrawable) {
        ((void(*)(id, SEL, id))s_original_presentDrawable)(self, _cmd, drawable);
    }
}

static void hooked_presentDrawableOptions(id self, SEL _cmd,
                                          id<CAMetalDrawable> drawable,
                                          NSUInteger options) {
    if (drawable) {
        arm_commit_hook_for(self);
        imgui_metal_record_present(self, drawable);
    }
    if (s_original_presentDrawableOptions) {
        ((void(*)(id, SEL, id, NSUInteger))s_original_presentDrawableOptions)(
            self, _cmd, drawable, options);
    }
}

static void hooked_commit(id self, SEL _cmd) {
    id<CAMetalDrawable> drawable = imgui_metal_take_present(self);

    s_diag.commits++;
    if (drawable) {
        s_diag.matched++;
        if (!imgui_metal_should_render()) {
            s_diag.skip_state++;
        } else if (!drawable.texture) {
            s_diag.skip_texture++;
        } else if (!imgui_metal_drawable_is_game_layer(drawable)) {
            s_diag.skip_layer++;
        } else {
            s_diag.rendered++;
            imgui_metal_render_frame(drawable, (id<MTLCommandBuffer>)self);
        }
    }

    if ((s_diag.commits % 600) == 0) imgui_metal_log_diag();

    if (s_original_commit) {
        ((void(*)(id, SEL))s_original_commit)(self, _cmd);
    }
}

// Swizzle presentDrawable: and commit on _MTLCommandBuffer, the Metal-framework
// class that implements them for every concrete (AGX*) command buffer subclass.
static void install_command_buffer_hook(void) {
    if (s_cmdBufHooked) return;

    Class cls = NSClassFromString(@"_MTLCommandBuffer");
    if (!cls) {
        LOG_IMGUI_WARN("_MTLCommandBuffer class not found; ImGui falls back to "
                       "the drawable present hook");
        return;
    }

    Method m = class_getInstanceMethod(cls, @selector(presentDrawable:));
    if (m) {
        s_original_presentDrawable = method_getImplementation(m);
        method_setImplementation(m, (IMP)hooked_presentDrawable);
    }

    Method mo = class_getInstanceMethod(cls, @selector(presentDrawable:options:));
    if (mo) {
        s_original_presentDrawableOptions = method_getImplementation(mo);
        method_setImplementation(mo, (IMP)hooked_presentDrawableOptions);
    }

    if (!s_original_presentDrawable && !s_original_presentDrawableOptions) {
        LOG_IMGUI_WARN("No presentDrawable: method on _MTLCommandBuffer; ImGui "
                       "falls back to the drawable present hook");
        return;
    }

    s_cmdBufHooked = true;
    LOG_IMGUI_INFO("Hooked presentDrawable: on _MTLCommandBuffer; -commit arms "
                   "on the first frame (render-thread ImGui path active)");
}

static void hooked_present(id self, SEL _cmd) {
    // With the command-buffer hook armed the overlay is already encoded on the
    // render thread; rendering again here would draw it twice and would do so
    // on Metal's completion queue.
    if (s_cmdBufHooked) {
        if (s_state.original_present) {
            ((void(*)(id, SEL))s_state.original_present)(self, _cmd);
        }
        return;
    }

    // Render ImGui BEFORE presenting (so it appears on top of game content).
    // Render when the F11 overlay is up OR any mod window is visible (MCM etc.).
    if (s_state.state == IMGUI_METAL_STATE_READY &&
        (s_state.visible || imgui_metal_has_visible_window())) {
        id<CAMetalDrawable> drawable = (id<CAMetalDrawable>)self;
        // Only render into the game's MAIN layer. During cutscenes/movies and
        // other transitions the game presents drawables from a different
        // CAMetalLayer; rendering our ImGui pass (sized/formatted for the main
        // layer) into those causes a GPU fault. Skip foreign drawables.
        if (s_state.gameLayer != nil && drawable.layer != s_state.gameLayer) {
            if (s_state.original_present) {
                ((void(*)(id, SEL))s_state.original_present)(self, _cmd);
            }
            return;
        }
        imgui_metal_render_frame(drawable, nil);

        // Log every 60 frames to confirm rendering is happening
        if (s_state.frame_count % 60 == 0) {
            LOG_IMGUI_DEBUG("Rendered frame %llu (via present hook)", s_state.frame_count);
        }
    }

    // Call original present
    if (s_state.original_present) {
        ((void(*)(id, SEL))s_state.original_present)(self, _cmd);
    }
}

static void install_present_hook_for_drawable(id<CAMetalDrawable> drawable) {
    if (s_presentHooked) {
        return;  // Already hooked
    }

    // Get the concrete class of the drawable
    Class drawableClass = object_getClass(drawable);
    if (!drawableClass) {
        LOG_IMGUI_ERROR("Could not get drawable class");
        return;
    }

    s_drawableClass = drawableClass;

    // Hook the present method
    Method presentMethod = class_getInstanceMethod(drawableClass, @selector(present));
    if (!presentMethod) {
        LOG_IMGUI_ERROR("Could not find present method on %s", class_getName(drawableClass));
        return;
    }

    s_state.original_present = method_getImplementation(presentMethod);
    method_setImplementation(presentMethod, (IMP)hooked_present);

    s_presentHooked = true;
    LOG_IMGUI_INFO("Hooked present method on %s", class_getName(drawableClass));
}

// ============================================================================
// NextDrawable Hook
// ============================================================================

static id<CAMetalDrawable> hooked_nextDrawable(id self, SEL _cmd) {
    // Safety check: ensure original method is available
    IMP originalImp = s_state.original_nextDrawable;
    if (!originalImp) {
        return nil;
    }

    CAMetalLayer *layer = (CAMetalLayer *)self;

    // Call original to get the drawable
    id<CAMetalDrawable> drawable = ((id<CAMetalDrawable>(*)(id, SEL))originalImp)(self, _cmd);

    if (!drawable) {
        return drawable;
    }

    // First time seeing a Metal layer - capture device and defer ImGui init
    if (s_state.state == IMGUI_METAL_STATE_WAITING_FOR_DEVICE) {
        if (!s_state.device && layer.device) {
            s_first_drawable_seen = true;
            s_state.device = layer.device;
            s_state.gameLayer = layer;

            LOG_IMGUI_INFO("Captured Metal device: %s",
                       [[s_state.device name] UTF8String]);

            // Defer ImGui context setup to main thread for safety
            dispatch_async(dispatch_get_main_queue(), ^{
                if (s_state.state == IMGUI_METAL_STATE_WAITING_FOR_DEVICE) {
                    // Find the game window on main thread
                    NSArray *windows = [NSApp windows];
                    for (NSWindow *window in windows) {
                        NSView *contentView = [window contentView];
                        if (contentView && [contentView layer] == s_state.gameLayer) {
                            s_state.gameWindow = window;
                            LOG_IMGUI_INFO("Found game window: %s",
                                       [[window title] UTF8String]);
                            break;
                        }
                    }

                    s_state.state = IMGUI_METAL_STATE_INITIALIZING;
                    imgui_metal_setup_context();
                }
            });
        }
    }

    // Hook the present method on this drawable class (once)
    if (s_state.state == IMGUI_METAL_STATE_READY && !s_presentHooked) {
        install_present_hook_for_drawable(drawable);
    }

    // Preferred path; leaves the drawable hook installed but inert.
    if (s_state.state == IMGUI_METAL_STATE_READY && !s_cmdBufHooked) {
        install_command_buffer_hook();
    }

    // Store current drawable for reference
    s_state.currentDrawable = drawable;

    return drawable;
}

// ============================================================================
// ImGui Setup
// ============================================================================

static void imgui_metal_setup_context(void) {
    LOG_IMGUI_INFO("Setting up ImGui context...");

    // Create command queue
    s_state.commandQueue = [s_state.device newCommandQueue];
    if (!s_state.commandQueue) {
        LOG_IMGUI_ERROR("Failed to create command queue");
        s_state.state = IMGUI_METAL_STATE_ERROR;
        return;
    }

    // Create ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // Enable keyboard nav

    // Set up display size (will be updated each frame)
    if (s_state.gameLayer) {
        CGSize size = s_state.gameLayer.drawableSize;
        io.DisplaySize = ImVec2(size.width, size.height);
    }

    // Set up style - dark theme for debug overlay
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 2.0f;
    style.Alpha = 0.95f;

    // Initialize Metal backend
    if (!ImGui_ImplMetal_Init(s_state.device)) {
        LOG_IMGUI_ERROR("ImGui_ImplMetal_Init failed");
        s_state.state = IMGUI_METAL_STATE_ERROR;
        return;
    }

    // Initialize macOS backend
    if (s_state.gameWindow) {
        NSView *view = [s_state.gameWindow contentView];
        if (!ImGui_ImplOSX_Init(view)) {
            LOG_IMGUI_ERROR("ImGui_ImplOSX_Init failed");
            s_state.state = IMGUI_METAL_STATE_ERROR;
            return;
        }
    }

    // Create render pass descriptor
    s_state.renderPassDescriptor = [MTLRenderPassDescriptor new];
    s_state.renderPassDescriptor.colorAttachments[0].loadAction = MTLLoadActionLoad;  // Preserve game content
    s_state.renderPassDescriptor.colorAttachments[0].storeAction = MTLStoreActionStore;

    // Create device objects (render pipeline state, font texture, etc.)
    // This must be done before first render
    LOG_IMGUI_INFO("Creating ImGui Metal device objects...");
    if (!ImGui_ImplMetal_CreateDeviceObjects(s_state.device)) {
        LOG_IMGUI_ERROR("Failed to create ImGui Metal device objects");
        s_state.state = IMGUI_METAL_STATE_ERROR;
        return;
    }
    LOG_IMGUI_INFO("Device objects created successfully");

    s_state.state = IMGUI_METAL_STATE_READY;
    s_state.frame_count = 0;

    // Apply pending visibility if Show() was called before ready
    if (s_state.pending_visible) {
        s_state.visible = true;
        s_state.pending_visible = false;
        LOG_IMGUI_INFO("Applied pending visibility - overlay now visible");
    } else {
        s_state.visible = false;
    }

    LOG_IMGUI_INFO("ImGui Metal backend initialized successfully");

    // Initialize input hooks on the game view
    if (s_state.gameWindow) {
        NSView *view = [s_state.gameWindow contentView];
        if (view) {
            imgui_input_hooks_init((__bridge void *)view);
        }
    }
}

// ============================================================================
// Widget Rendering
// ============================================================================

// Forward declaration for recursive rendering
static void render_widget(ImguiObject *obj);

// Push style overrides for an object
void imgui_object_push_style(ImguiObject* obj) {
    if (!obj) return;

    // Push style vars. ImGui asserts if you push the wrong arity (ImVec2 for a
    // float var or vice versa), so dispatch by the var's type for THIS imgui.h
    // build. The ImVec2 vars (indices) are: WindowPadding(2), WindowMinSize(5),
    // WindowTitleAlign(6), FramePadding(11), ItemSpacing(14), ItemInnerSpacing(15),
    // CellPadding(17), TableAngledHeadersTextAlign(31), ButtonTextAlign(34),
    // SelectableTextAlign(35), SeparatorTextAlign(37), SeparatorTextPadding(38).
    for (int i = 0; i < obj->style_overrides.style_count; i++) {
        int var = obj->style_overrides.style_vars[i];
        float val1 = obj->style_overrides.style_values[i * 2];
        float val2 = obj->style_overrides.style_values[i * 2 + 1];

        switch (var) {
            case 2: case 5: case 6: case 11: case 14: case 15:
            case 17: case 31: case 34: case 35: case 37: case 38:
                ImGui::PushStyleVar((ImGuiStyleVar)var, ImVec2(val1, val2));
                break;
            default:
                ImGui::PushStyleVar((ImGuiStyleVar)var, val1);
                break;
        }
    }

    // Push style colors
    for (int i = 0; i < obj->style_overrides.color_count; i++) {
        int color = obj->style_overrides.color_vars[i];
        ImguiVec4 val = obj->style_overrides.color_values[i];
        ImGui::PushStyleColor((ImGuiCol)color, ImVec4(val.x, val.y, val.z, val.w));
    }
}

// Pop style overrides for an object
void imgui_object_pop_style(ImguiObject* obj) {
    if (!obj) return;

    // Pop in reverse order (colors first, then vars)
    if (obj->style_overrides.color_count > 0) {
        ImGui::PopStyleColor(obj->style_overrides.color_count);
    }
    if (obj->style_overrides.style_count > 0) {
        ImGui::PopStyleVar(obj->style_overrides.style_count);
    }
}

// Render a single widget based on its type
static void render_widget(ImguiObject *obj) {
    if (!obj || !obj->styled.visible) {
        return;
    }

    // Push style overrides before rendering
    imgui_object_push_style(obj);

    /*
     * IDContext scopes the widget's ImGui ID. Mods set it so widgets that share
     * a label stay distinct -- ImGui keys state by label, so without it two
     * buttons named the same are the same button, and a click on one operates
     * the other. MCM sets it on almost everything it builds.
     */
    bool pushed_id = false;
    if (obj->styled.id_context[0] != '\0') {
        ImGui::PushID(obj->styled.id_context);
        pushed_id = true;
    }

    // Seed a unique ImGui ID per object (its handle). Widgets often have empty
    // labels (e.g. MCM buttons whose text comes from a missing localization
    // string, which we tolerate as ""); an empty label at a window root makes
    // ImGui compute an item ID equal to the window's own ID and fire the
    // "Cannot have an empty ID at the root of a window" assertion -> abort. A
    // per-handle PushID makes every item's ID unique regardless of its label.
    ImGui::PushID((void *)(uintptr_t)obj->handle);

    // Handle SameLine
    if (obj->styled.same_line) {
        ImGui::SameLine();
    }

    // Render based on type
    switch (obj->type) {
        case IMGUI_OBJ_TEXT:
            if (obj->data.text.has_color) {
                ImGui::TextColored(
                    ImVec4(obj->data.text.color.x, obj->data.text.color.y,
                           obj->data.text.color.z, obj->data.text.color.w),
                    "%s", obj->styled.label);
            } else {
                ImGui::Text("%s", obj->styled.label);
            }
            break;

        case IMGUI_OBJ_BUTTON:
            {
                ImVec2 size(0, 0);
                if (obj->data.button.has_size) {
                    size = ImVec2(obj->data.button.size.x, obj->data.button.size.y);
                }
                if (ImGui::Button(obj->styled.label, size)) {
                    LOG_IMGUI_DEBUG("Button '%s' clicked", obj->styled.label);
                    lua_imgui_fire_event(obj->handle, IMGUI_EVENT_ON_CLICK);
                }
            }
            break;

        case IMGUI_OBJ_CHECKBOX:
            {
                bool checked = obj->data.checkbox.checked;
                if (ImGui::Checkbox(obj->styled.label, &checked)) {
                    obj->data.checkbox.checked = checked;
                    LOG_IMGUI_DEBUG("Checkbox '%s' changed to %d", obj->styled.label, checked);
                    lua_imgui_fire_event(obj->handle, IMGUI_EVENT_ON_CHANGE, (int)checked);
                }
            }
            break;

        case IMGUI_OBJ_SEPARATOR:
            ImGui::Separator();
            break;

        case IMGUI_OBJ_SPACING:
            ImGui::Spacing();
            break;

        case IMGUI_OBJ_NEW_LINE:
            ImGui::NewLine();
            break;

        case IMGUI_OBJ_DUMMY:
            ImGui::Dummy(ImVec2(obj->data.dummy.width, obj->data.dummy.height));
            break;

        case IMGUI_OBJ_GROUP:
            // Group is a container - render children
            ImGui::BeginGroup();
            if (obj->children && obj->child_count > 0) {
                for (int i = 0; i < obj->child_count; i++) {
                    ImguiObject *child = imgui_object_get(obj->children[i]);
                    if (child) render_widget(child);
                }
            }
            ImGui::EndGroup();
            break;

        case IMGUI_OBJ_INPUT_TEXT:
            {
                char buf[4096];
                strncpy(buf, obj->data.input_text.text, sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = '\0';

                ImGuiInputTextFlags flags = (ImGuiInputTextFlags)obj->data.input_text.flags;
                bool changed = false;

                if (obj->data.input_text.hint[0]) {
                    changed = ImGui::InputTextWithHint(obj->styled.label, obj->data.input_text.hint,
                                                        buf, sizeof(buf), flags);
                } else {
                    changed = ImGui::InputText(obj->styled.label, buf, sizeof(buf), flags);
                }

                if (changed) {
                    strncpy(obj->data.input_text.text, buf, sizeof(obj->data.input_text.text) - 1);
                    obj->data.input_text.text[sizeof(obj->data.input_text.text) - 1] = '\0';
                    lua_imgui_fire_event(obj->handle, IMGUI_EVENT_ON_CHANGE, 0);
                }
            }
            break;

        case IMGUI_OBJ_COMBO:
            {
                const char* preview = (obj->data.combo.selected_index >= 0 &&
                                       obj->data.combo.selected_index < obj->data.combo.option_count)
                    ? obj->data.combo.options[obj->data.combo.selected_index]
                    : "";

                if (ImGui::BeginCombo(obj->styled.label, preview, (ImGuiComboFlags)obj->data.combo.flags)) {
                    for (int i = 0; i < obj->data.combo.option_count; i++) {
                        bool is_selected = (obj->data.combo.selected_index == i);
                        if (ImGui::Selectable(obj->data.combo.options[i], is_selected)) {
                            obj->data.combo.selected_index = i;
                            lua_imgui_fire_event(obj->handle, IMGUI_EVENT_ON_CHANGE, i + 1);  // 1-indexed for Lua
                        }
                        if (is_selected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
            }
            break;

        case IMGUI_OBJ_SLIDER_SCALAR:
            {
                float val = obj->data.slider.value.x;
                if (ImGui::SliderFloat(obj->styled.label, &val,
                                       obj->data.slider.min.x, obj->data.slider.max.x)) {
                    obj->data.slider.value.x = val;
                    lua_imgui_fire_event(obj->handle, IMGUI_EVENT_ON_CHANGE, 0);
                }
            }
            break;

        case IMGUI_OBJ_SLIDER_INT:
            {
                int val = obj->data.slider_int.value[0];
                if (ImGui::SliderInt(obj->styled.label, &val,
                                     obj->data.slider_int.min[0], obj->data.slider_int.max[0])) {
                    obj->data.slider_int.value[0] = val;
                    lua_imgui_fire_event(obj->handle, IMGUI_EVENT_ON_CHANGE, val);
                }
            }
            break;

        case IMGUI_OBJ_COLLAPSING_HEADER:
            {
                ImGuiTreeNodeFlags flags = (ImGuiTreeNodeFlags)obj->data.collapsing_header.flags;
                bool is_open = ImGui::CollapsingHeader(obj->styled.label, flags);
                obj->data.collapsing_header.is_open = is_open;

                // Render children if open
                if (is_open && obj->children && obj->child_count > 0) {
                    for (int i = 0; i < obj->child_count; i++) {
                        ImguiObject *child = imgui_object_get(obj->children[i]);
                        if (child) render_widget(child);
                    }
                }
            }
            break;

        case IMGUI_OBJ_BULLET_TEXT:
            ImGui::BulletText("%s", obj->styled.label);
            break;

        case IMGUI_OBJ_SEPARATOR_TEXT:
            ImGui::SeparatorText(obj->styled.label);
            break;

        case IMGUI_OBJ_PROGRESS_BAR:
            {
                const char *overlay = obj->data.progress_bar.overlay[0]
                    ? obj->data.progress_bar.overlay
                    : NULL;
                ImGui::ProgressBar(obj->data.progress_bar.value,
                                   ImVec2(obj->data.progress_bar.size.x, obj->data.progress_bar.size.y),
                                   overlay);
            }
            break;

        case IMGUI_OBJ_IMAGE:
            {
                // TODO: Implement texture loading from image_path
                // For now, render as placeholder text showing image info
                char placeholder[512];
                snprintf(placeholder, sizeof(placeholder), "[Image: %s (%.0fx%.0f)]",
                         obj->data.image.image_path,
                         obj->data.image.size.x,
                         obj->data.image.size.y);
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s", placeholder);

                // When texture loading is implemented:
                // ImTextureID texId = imgui_texture_get(obj->data.image.image_path);
                // if (texId) {
                //     ImGui::Image(texId, ImVec2(obj->data.image.size.x, obj->data.image.size.y),
                //                  ImVec2(obj->data.image.uv0.x, obj->data.image.uv0.y),
                //                  ImVec2(obj->data.image.uv1.x, obj->data.image.uv1.y),
                //                  ImVec4(obj->data.image.tint.x, obj->data.image.tint.y,
                //                         obj->data.image.tint.z, obj->data.image.tint.w),
                //                  ImVec4(obj->data.image.border.x, obj->data.image.border.y,
                //                         obj->data.image.border.z, obj->data.image.border.w));
                // }
            }
            break;

        case IMGUI_OBJ_RADIO_BUTTON:
            {
                bool active = obj->data.radio_button.active;
                if (ImGui::RadioButton(obj->styled.label, active)) {
                    obj->data.radio_button.active = !active;
                    lua_imgui_fire_event(obj->handle, IMGUI_EVENT_ON_CLICK);
                }
            }
            break;

        case IMGUI_OBJ_COLOR_EDIT:
            {
                float col[4] = {
                    obj->data.color.color.x,
                    obj->data.color.color.y,
                    obj->data.color.color.z,
                    obj->data.color.color.w
                };
                if (ImGui::ColorEdit4(obj->styled.label, col, (ImGuiColorEditFlags)obj->data.color.flags)) {
                    obj->data.color.color.x = col[0];
                    obj->data.color.color.y = col[1];
                    obj->data.color.color.z = col[2];
                    obj->data.color.color.w = col[3];
                    lua_imgui_fire_event(obj->handle, IMGUI_EVENT_ON_CHANGE, 0);
                }
            }
            break;

        case IMGUI_OBJ_COLOR_PICKER:
            {
                float col[4] = {
                    obj->data.color.color.x,
                    obj->data.color.color.y,
                    obj->data.color.color.z,
                    obj->data.color.color.w
                };
                if (ImGui::ColorPicker4(obj->styled.label, col, (ImGuiColorEditFlags)obj->data.color.flags)) {
                    obj->data.color.color.x = col[0];
                    obj->data.color.color.y = col[1];
                    obj->data.color.color.z = col[2];
                    obj->data.color.color.w = col[3];
                    lua_imgui_fire_event(obj->handle, IMGUI_EVENT_ON_CHANGE, 0);
                }
            }
            break;

        case IMGUI_OBJ_DRAG_SCALAR:
            {
                float val = obj->data.slider.value.x;
                if (ImGui::DragFloat(obj->styled.label, &val,
                                     0.1f, obj->data.slider.min.x, obj->data.slider.max.x)) {
                    obj->data.slider.value.x = val;
                    lua_imgui_fire_event(obj->handle, IMGUI_EVENT_ON_CHANGE, 0);
                }
            }
            break;

        case IMGUI_OBJ_DRAG_INT:
            {
                int val = obj->data.slider_int.value[0];
                if (ImGui::DragInt(obj->styled.label, &val,
                                   1.0f, obj->data.slider_int.min[0], obj->data.slider_int.max[0])) {
                    obj->data.slider_int.value[0] = val;
                    lua_imgui_fire_event(obj->handle, IMGUI_EVENT_ON_CHANGE, val);
                }
            }
            break;

        case IMGUI_OBJ_INPUT_SCALAR:
            {
                float val = obj->data.slider.value.x;
                if (ImGui::InputFloat(obj->styled.label, &val, 0.0f, 0.0f, "%.3f",
                                      (ImGuiInputTextFlags)obj->data.slider.flags)) {
                    obj->data.slider.value.x = val;
                    lua_imgui_fire_event(obj->handle, IMGUI_EVENT_ON_CHANGE, 0);
                }
            }
            break;

        case IMGUI_OBJ_INPUT_INT:
            {
                int val = obj->data.slider_int.value[0];
                if (ImGui::InputInt(obj->styled.label, &val, 1, 100,
                                    (ImGuiInputTextFlags)obj->data.slider_int.flags)) {
                    obj->data.slider_int.value[0] = val;
                    lua_imgui_fire_event(obj->handle, IMGUI_EVENT_ON_CHANGE, val);
                }
            }
            break;

        case IMGUI_OBJ_SELECTABLE:
            {
                bool selected = obj->data.selectable.selected;
                ImVec2 size(0, 0);
                if (obj->data.selectable.has_size) {
                    size = ImVec2(obj->data.selectable.size.x, obj->data.selectable.size.y);
                }
                if (ImGui::Selectable(obj->styled.label, &selected,
                                      (ImGuiSelectableFlags)obj->data.selectable.flags, size)) {
                    obj->data.selectable.selected = selected;
                    lua_imgui_fire_event(obj->handle, IMGUI_EVENT_ON_CLICK);
                }
            }
            break;

        case IMGUI_OBJ_TEXT_LINK:
            if (ImGui::TextLink(obj->styled.label)) {
                lua_imgui_fire_event(obj->handle, IMGUI_EVENT_ON_CLICK);
            }
            break;

        case IMGUI_OBJ_TREE:
            {
                ImGuiTreeNodeFlags flags = (ImGuiTreeNodeFlags)obj->data.tree.flags;
                bool was_open = obj->data.tree.is_open;
                obj->data.tree.is_open = ImGui::TreeNodeEx(obj->styled.label, flags);

                if (obj->data.tree.is_open != was_open) {
                    if (obj->data.tree.is_open) {
                        lua_imgui_fire_event(obj->handle, IMGUI_EVENT_ON_EXPAND);
                    } else {
                        lua_imgui_fire_event(obj->handle, IMGUI_EVENT_ON_COLLAPSE);
                    }
                }

                if (obj->data.tree.is_open) {
                    // Render children
                    if (obj->children && obj->child_count > 0) {
                        for (int i = 0; i < obj->child_count; i++) {
                            ImguiObject *child = imgui_object_get(obj->children[i]);
                            if (child) render_widget(child);
                        }
                    }
                    ImGui::TreePop();
                }
            }
            break;

        case IMGUI_OBJ_TABLE:
            {
                ImguiTableData* d = &obj->data.table;
                ImVec2 size = d->has_size ? ImVec2(d->size.x, d->size.y) : ImVec2(0, 0);

                if (ImGui::BeginTable(obj->styled.label, d->columns, (ImGuiTableFlags)d->flags, size)) {
                    // Declared columns first: TableSetupColumn must precede any
                    // row content, and this is what carries a column's width.
                    for (int c = 0; c < d->col_def_count; c++) {
                        const ImguiColumnDef *cd = &d->col_defs[c];
                        ImGui::TableSetupColumn(cd->name,
                                                (ImGuiTableColumnFlags)cd->flags,
                                                cd->width);
                    }
                    if (d->freeze_cols > 0 || d->freeze_rows > 0) {
                        ImGui::TableSetupScrollFreeze(d->freeze_cols, d->freeze_rows);
                    }

                    // Render children (TableRow widgets)
                    if (obj->children && obj->child_count > 0) {
                        for (int i = 0; i < obj->child_count; i++) {
                            ImguiObject *child = imgui_object_get(obj->children[i]);
                            if (child) render_widget(child);
                        }
                    }

                    // Check for sort changes
                    ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs();
                    if (specs && specs->SpecsDirty) {
                        lua_imgui_fire_event(obj->handle, IMGUI_EVENT_ON_SORT_CHANGED);
                        specs->SpecsDirty = false;
                    }

                    ImGui::EndTable();
                }
            }
            break;

        case IMGUI_OBJ_TABLE_ROW:
            {
                ImGui::TableNextRow((ImGuiTableRowFlags)obj->data.table_row.flags);
                // Render children (TableCell widgets)
                if (obj->children && obj->child_count > 0) {
                    for (int i = 0; i < obj->child_count; i++) {
                        ImguiObject *child = imgui_object_get(obj->children[i]);
                        if (child) render_widget(child);
                    }
                }
            }
            break;

        case IMGUI_OBJ_TABLE_CELL:
            {
                ImGui::TableNextColumn();
                // Render children within this cell
                if (obj->children && obj->child_count > 0) {
                    for (int i = 0; i < obj->child_count; i++) {
                        ImguiObject *child = imgui_object_get(obj->children[i]);
                        if (child) render_widget(child);
                    }
                }
            }
            break;

        case IMGUI_OBJ_TAB_BAR:
            {
                if (ImGui::BeginTabBar(obj->styled.label, (ImGuiTabBarFlags)obj->data.tab_bar.flags)) {
                    // Render children (TabItem widgets)
                    if (obj->children && obj->child_count > 0) {
                        for (int i = 0; i < obj->child_count; i++) {
                            ImguiObject *child = imgui_object_get(obj->children[i]);
                            if (child) render_widget(child);
                        }
                    }
                    ImGui::EndTabBar();
                }
            }
            break;

        case IMGUI_OBJ_TAB_ITEM:
            {
                ImguiTabItemData* d = &obj->data.tab_item;
                bool was_selected = d->is_selected;

                if (ImGui::BeginTabItem(obj->styled.label, NULL, (ImGuiTabItemFlags)d->flags)) {
                    d->is_selected = true;
                    // Render children
                    if (obj->children && obj->child_count > 0) {
                        for (int i = 0; i < obj->child_count; i++) {
                            ImguiObject *child = imgui_object_get(obj->children[i]);
                            if (child) render_widget(child);
                        }
                    }
                    ImGui::EndTabItem();
                } else {
                    d->is_selected = false;
                }

                if (d->is_selected != was_selected && d->is_selected) {
                    lua_imgui_fire_event(obj->handle, IMGUI_EVENT_ON_ACTIVATE);
                }
            }
            break;

        case IMGUI_OBJ_MENU_BAR:
            {
                if (ImGui::BeginMenuBar()) {
                    // Render children (Menu widgets)
                    if (obj->children && obj->child_count > 0) {
                        for (int i = 0; i < obj->child_count; i++) {
                            ImguiObject *child = imgui_object_get(obj->children[i]);
                            if (child) render_widget(child);
                        }
                    }
                    ImGui::EndMenuBar();
                }
            }
            break;

        case IMGUI_OBJ_MENU:
            {
                if (ImGui::BeginMenu(obj->styled.label)) {
                    obj->data.menu.is_open = true;
                    // Render children (MenuItem widgets)
                    if (obj->children && obj->child_count > 0) {
                        for (int i = 0; i < obj->child_count; i++) {
                            ImguiObject *child = imgui_object_get(obj->children[i]);
                            if (child) render_widget(child);
                        }
                    }
                    ImGui::EndMenu();
                } else {
                    obj->data.menu.is_open = false;
                }
            }
            break;

        case IMGUI_OBJ_MENU_ITEM:
            {
                ImguiMenuItemData* d = &obj->data.menu_item;
                const char* shortcut = d->shortcut[0] ? d->shortcut : NULL;
                if (ImGui::MenuItem(obj->styled.label, shortcut, false, d->enabled)) {
                    lua_imgui_fire_event(obj->handle, IMGUI_EVENT_ON_CLICK);
                }
            }
            break;

        case IMGUI_OBJ_POPUP:
            {
                ImguiPopupData* d = &obj->data.popup;
                if (d->is_open) {
                    if (ImGui::BeginPopup(obj->styled.label, (ImGuiWindowFlags)d->flags)) {
                        // Render children
                        if (obj->children && obj->child_count > 0) {
                            for (int i = 0; i < obj->child_count; i++) {
                                ImguiObject *child = imgui_object_get(obj->children[i]);
                                if (child) render_widget(child);
                            }
                        }
                        ImGui::EndPopup();
                    } else {
                        d->is_open = false;
                    }
                }
            }
            break;

        case IMGUI_OBJ_TOOLTIP:
            {
                if (ImGui::BeginTooltip()) {
                    // Render children
                    if (obj->children && obj->child_count > 0) {
                        for (int i = 0; i < obj->child_count; i++) {
                            ImguiObject *child = imgui_object_get(obj->children[i]);
                            if (child) render_widget(child);
                        }
                    }
                    ImGui::EndTooltip();
                }
            }
            break;

        case IMGUI_OBJ_CHILD_WINDOW:
            {
                ImguiChildWindowData* d = &obj->data.child_window;
                ImVec2 size = d->has_size ? ImVec2(d->size.x, d->size.y) : ImVec2(0, 0);

                if (ImGui::BeginChild(obj->styled.label, size, (ImGuiChildFlags)d->child_flags,
                                      (ImGuiWindowFlags)d->flags)) {
                    // Render children
                    if (obj->children && obj->child_count > 0) {
                        for (int i = 0; i < obj->child_count; i++) {
                            ImguiObject *child = imgui_object_get(obj->children[i]);
                            if (child) render_widget(child);
                        }
                    }
                }
                ImGui::EndChild();
            }
            break;

        default:
            // Unknown type - skip
            break;
    }

    // Track hover state changes for all widgets
    bool is_hovered = ImGui::IsItemHovered();
    if (is_hovered && !obj->styled.was_hovered) {
        lua_imgui_fire_event(obj->handle, IMGUI_EVENT_ON_HOVER_ENTER);
    }
    if (!is_hovered && obj->styled.was_hovered) {
        lua_imgui_fire_event(obj->handle, IMGUI_EVENT_ON_HOVER_LEAVE);
    }
    obj->styled.was_hovered = is_hovered;

    // Track activate/deactivate
    if (ImGui::IsItemActivated()) {
        lua_imgui_fire_event(obj->handle, IMGUI_EVENT_ON_ACTIVATE);
    }
    if (ImGui::IsItemDeactivated()) {
        lua_imgui_fire_event(obj->handle, IMGUI_EVENT_ON_DEACTIVATE);
    }

    ImGui::PopID();

    // Pop style overrides after rendering
    if (pushed_id) {
        ImGui::PopID();
    }
    imgui_object_pop_style(obj);
}

// Render a window and its children
static void render_window(ImguiObject *win) {
    if (!win || win->type != IMGUI_OBJ_WINDOW || !win->styled.visible) {
        return;
    }

    if (!win->data.window.open) {
        return;
    }

    // Build window flags
    ImGuiWindowFlags flags = (ImGuiWindowFlags)win->data.window.flags;

    // Closeable handling
    bool *p_open = win->data.window.closeable ? &win->data.window.open : NULL;

    // Set initial position/size for new windows
    ImGui::SetNextWindowPos(ImVec2(550, 100), ImGuiCond_FirstUseEver);  // Right of debug window
    ImGui::SetNextWindowSize(ImVec2(400, 500), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(1.0f);  // Fully opaque
    ImGui::SetNextWindowCollapsed(false, ImGuiCond_FirstUseEver);

    // Begin window
    bool window_open = ImGui::Begin(win->styled.label, p_open, flags);

    static int render_log_counter = 0;
    if (render_log_counter++ % 300 == 0) {
        LOG_IMGUI_DEBUG("render_window: '%s' Begin returned %d, children=%d, flags=0x%x",
            win->styled.label, window_open, win->child_count, flags);
    }

    if (window_open) {
        // Record geometry while the window is current; mods read LastPosition
        // and LastSize to persist placement.
        ImVec2 wpos = ImGui::GetWindowPos();
        ImVec2 wsize = ImGui::GetWindowSize();
        win->data.window.last_position.x = wpos.x;
        win->data.window.last_position.y = wpos.y;
        win->data.window.last_size.x = wsize.x;
        win->data.window.last_size.y = wsize.y;
        win->data.window.has_last_geometry = true;

        // Render all children
        if (win->children && win->child_count > 0) {
            for (int i = 0; i < win->child_count; i++) {
                ImguiObject *child = imgui_object_get(win->children[i]);
                if (child) render_widget(child);
            }
        }
    }

    ImGui::End();

    // Handle window close event (X button set data.window.open = false).
    // Keep .Visible in sync so the window actually disappears and MCM's OnClose
    // handler sees a consistent state. Either setter reopens it.
    if (p_open && !win->data.window.open) {
        win->styled.visible = false;
        LOG_IMGUI_DEBUG("Window '%s' closed", win->styled.label);
        lua_imgui_fire_event(win->handle, IMGUI_EVENT_ON_CLOSE);
    }
}

// ============================================================================
// Public Render API (for standalone test apps)
// ============================================================================

void imgui_metal_render_all_windows(void) {
    // Hold the pool lock across iteration — see imgui_metal_has_visible_window.
    imgui_objects_lock();
    int window_count = 0;
    ImguiHandle *windows = imgui_get_all_windows(&window_count);

    if (windows && window_count > 0) {
        for (int i = 0; i < window_count; i++) {
            ImguiObject *win = imgui_object_get(windows[i]);
            render_window(win);
        }
    }
    imgui_objects_unlock();
}

// ============================================================================
// Rendering
// ============================================================================

static void imgui_metal_render_frame(id<CAMetalDrawable> drawable,
                                     id<MTLCommandBuffer> gameCommandBuffer) {
    // Safety checks. commandQueue is only needed for the legacy own-queue path.
    if (!drawable || !s_state.device || !s_state.renderPassDescriptor) {
        return;
    }
    if (!gameCommandBuffer && !s_state.commandQueue) {
        return;
    }

    @autoreleasepool {
        @try {
            // Set up render pass with drawable texture FIRST
            // (ImGui_ImplMetal_NewFrame needs the texture to get pixel format)
            s_state.renderPassDescriptor.colorAttachments[0].texture = drawable.texture;

            // Update display size using POINTS (not pixels) to match ImGui_ImplOSX_NewFrame
            // CGEventTap coordinates are in points, so DisplaySize must be in points too
            if (s_state.gameWindow) {
                NSView *view = [s_state.gameWindow contentView];
                if (view) {
                    CGSize viewSize = view.bounds.size;
                    if (viewSize.width <= 0 || viewSize.height <= 0) {
                        return;  // Invalid size, skip this frame
                    }
                    ImGuiIO& io = ImGui::GetIO();
                    io.DisplaySize = ImVec2(viewSize.width, viewSize.height);

                    // Retina scale for framebuffer (points to pixels)
                    CGFloat scale = s_state.gameLayer.contentsScale;
                    io.DisplayFramebufferScale = ImVec2(scale, scale);
                }
            }

            // Start ImGui frame (needs texture set above for pixel format)
            ImGui_ImplMetal_NewFrame(s_state.renderPassDescriptor);
            // NOTE: We skip ImGui_ImplOSX_NewFrame because:
            // 1. It overwrites mouse position with Cocoa APIs (doesn't work for SDL)
            // 2. We handle mouse input via CGEventTap instead
            // 3. It may be causing coordinate system mismatches

            // CRITICAL: Apply CGEventTap mouse position
            // The OSX backend overwrites io.MousePos with Cocoa's mouse location,
            // which doesn't work for SDL games. Our CGEventTap position is correct.
            // We set MousePos directly because AddMousePosEvent queues for next frame.
            {
                ImGuiIO& io = ImGui::GetIO();
                if (s_cgevent_mouse.valid) {
                    io.MousePos = ImVec2(s_cgevent_mouse.x, s_cgevent_mouse.y);
                    // Log occasionally to verify
                    static int applyCount = 0;
                    if (++applyCount % 120 == 0) {
                        LOG_IMGUI_DEBUG("Applied CGEventTap pos: (%.0f, %.0f)",
                            s_cgevent_mouse.x, s_cgevent_mouse.y);
                    }
                }
            }

            // Apply queued font loads / scale changes before the frame opens.
            imgui_metal_apply_pending_style();

            ImGui::NewFrame();

            // ===========================================
            // Render all windows from object system
            // ===========================================

            // Render Lua-created windows. We must hold the object-tree lock across
            // the walk so the main/Lua thread can't realloc/free a children array
            // (via Add*/Destroy) while we read it — that race corrupted the heap
            // and crashed the game's render thread. But the render thread must
            // NEVER block on the main thread (it drives present; a stall hangs the
            // game during load), so use a non-blocking acquire and simply skip the
            // object-tree render this frame if the main thread is mutating.
            static int debug_log_counter = 0;
            int window_count = 0;
            if (imgui_objects_trylock()) {
                ImguiHandle *windows = imgui_get_all_windows(&window_count);
                if (windows && window_count > 0) {
                    for (int i = 0; i < window_count; i++) {
                        ImguiObject *win = imgui_object_get(windows[i]);
                        if (debug_log_counter % 300 == 0) {  // Log every ~5 seconds
                            if (win) {
                                LOG_IMGUI_DEBUG("Window[%d]: label='%s' visible=%d open=%d type=%d children=%d",
                                    i, win->styled.label, win->styled.visible, win->data.window.open,
                                    win->type, win->child_count);
                            } else {
                                LOG_IMGUI_WARN("Window[%d]: NULL (handle=0x%llx)", i, (unsigned long long)windows[i]);
                            }
                        }
                        render_window(win);
                    }
                }
                imgui_objects_unlock();
            }
            debug_log_counter++;

            // Built-in debug/test window: only shown with the F11 debug overlay,
            // NOT when only a mod window (MCM) is visible. The mod-window loop
            // above renders independently (render_window skips non-visible ones).
            if (s_state.visible) {
                ImGui::SetNextWindowPos(ImVec2(100, 100), ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowSize(ImVec2(400, 300), ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowBgAlpha(1.0f);  // Fully opaque background

                if (ImGui::Begin("BG3SE Debug", nullptr, ImGuiWindowFlags_NoCollapse)) {
                    ImGui::TextColored(ImVec4(1,1,0,1), "=== IMGUI OVERLAY TEST ===");
                    ImGui::Text("Frame: %llu", s_state.frame_count);
                    ImGui::Separator();
                    ImGui::TextColored(ImVec4(0,1,0,1), "If you can see this, ImGui is working!");
                    ImGui::Text("Device: %s", [[s_state.device name] UTF8String]);

                    // Debug: Show mouse position and window bounds
                    ImGuiIO& dbgIo = ImGui::GetIO();
                    ImGui::Text("Mouse: (%.0f, %.0f)", dbgIo.MousePos.x, dbgIo.MousePos.y);
                    ImGui::Text("WantCapture: %d  MouseDown: %d", dbgIo.WantCaptureMouse, dbgIo.MouseDown[0]);
                    ImGui::Text("DisplaySize: %.0fx%.0f", dbgIo.DisplaySize.x, dbgIo.DisplaySize.y);

                    // Show Lua window count
                    ImGui::Separator();
                    ImGui::Text("Lua Windows: %d", window_count);

                    // Test button
                    ImGui::Separator();
                    if (ImGui::Button("Test Button")) {
                        LOG_IMGUI_INFO("Button clicked!");
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::TextColored(ImVec4(0,1,0,1), "HOVERING");
                    }
                }
                ImGui::End();
            }

            // Ext.IMGUI.EnableDemo(true) shows the stock ImGui demo window.
            // Independent of the F11 debug overlay, matching Windows where
            // EnableDemo is its own toggle.
            if (s_state.demo_enabled) {
                bool open = true;
                ImGui::ShowDemoWindow(&open);
                if (!open) {
                    s_state.demo_enabled = false;
                }
            }

            // End frame and render
            ImGui::Render();

            // Prefer the game's command buffer. Encoding into it keeps our
            // pass on the engine's render thread and orders it against the
            // game's own passes for free - no second queue, no stalling wait.
            id<MTLCommandBuffer> commandBuffer = gameCommandBuffer;
            if (!commandBuffer) {
                commandBuffer = [s_state.commandQueue commandBuffer];
            }
            if (!commandBuffer) {
                LOG_IMGUI_ERROR("Failed to create command buffer");
                return;
            }

            // Create render encoder (texture already set at start of function)
            id<MTLRenderCommandEncoder> renderEncoder =
                [commandBuffer renderCommandEncoderWithDescriptor:s_state.renderPassDescriptor];
            if (!renderEncoder) {
                LOG_IMGUI_ERROR("Failed to create render encoder");
                return;
            }

            // Render ImGui draw data
            ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), commandBuffer, renderEncoder);

            [renderEncoder endEncoding];

            if (gameCommandBuffer) {
                // The engine owns this buffer: it adds the present and commits.
                // Touching either here would present the frame twice.
                s_state.frame_count++;
                return;
            }

            // Legacy own-queue path. Surface GPU-side errors (drawable/texture
            // faults during scene transitions don't raise an ObjC exception;
            // they land here).
            [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> cb) {
                if (cb.status == MTLCommandBufferStatusError && cb.error) {
                    LOG_IMGUI_ERROR("ImGui cmd buffer GPU error: %s",
                                    [[cb.error localizedDescription] UTF8String]);
                }
            }];

            // Commit, then BLOCK until our GPU work has finished writing the
            // drawable's texture BEFORE returning to hooked_present (which then
            // calls the game's original present). Our command buffer runs on a
            // separate command queue than the game's; without this wait the game
            // can present/scan-out the drawable while our render pass is still
            // writing it, which faults the GPU driver and crashes WindowServer
            // (takes down the whole macOS compositor). waitUntilCompleted makes
            // the ordering deterministic and safe. It costs a per-frame stall,
            // but correctness/safety wins over the frame-time hit here.
            [commandBuffer commit];
            [commandBuffer waitUntilCompleted];

            s_state.frame_count++;
        }
        @catch (NSException *exception) {
            LOG_IMGUI_ERROR("Exception in ImGui render: %s - %s",
                [[exception name] UTF8String],
                [[exception reason] UTF8String]);
        }
    }
}

// ============================================================================
// Public API
// ============================================================================

bool imgui_metal_init(void) {
    if (s_state.state != IMGUI_METAL_STATE_UNINITIALIZED) {
        LOG_IMGUI_WARN("Metal backend already initialized");
        return true;
    }

    LOG_IMGUI_INFO("Initializing ImGui Metal backend...");
    install_layer_hook();

    return s_state.state != IMGUI_METAL_STATE_ERROR;
}

static void remove_command_buffer_hook(void) {
    if (!s_cmdBufHooked) return;

    Class cls = NSClassFromString(@"_MTLCommandBuffer");
    if (cls) {
        if (s_original_presentDrawable) {
            Method m = class_getInstanceMethod(cls, @selector(presentDrawable:));
            if (m) method_setImplementation(m, s_original_presentDrawable);
        }
        if (s_original_presentDrawableOptions) {
            Method mo = class_getInstanceMethod(cls, @selector(presentDrawable:options:));
            if (mo) method_setImplementation(mo, s_original_presentDrawableOptions);
        }
    }

    if (s_commitHooked && s_commitClass && s_original_commit) {
        IMP unused = NULL;
        swizzle_own_method(s_commitClass, @selector(commit), s_original_commit, &unused);
    }

    s_original_presentDrawable = NULL;
    s_original_presentDrawableOptions = NULL;
    s_original_commit = NULL;
    s_commitClass = nil;
    s_commitHooked = false;
    s_cmdBufHooked = false;
}

void imgui_metal_shutdown(void) {
    LOG_IMGUI_INFO("Shutting down ImGui Metal backend...");

    // Remove input hooks first
    imgui_input_hooks_shutdown();
    remove_command_buffer_hook();

    // Restore original method
    remove_layer_hook();

    // Clean up ImGui
    if (s_state.state == IMGUI_METAL_STATE_READY) {
        ImGui_ImplMetal_Shutdown();
        ImGui_ImplOSX_Shutdown();
        ImGui::DestroyContext();
    }

    // Release Metal objects
    s_state.commandQueue = nil;
    s_state.renderPassDescriptor = nil;
    s_state.device = nil;
    s_state.gameLayer = nil;
    s_state.gameWindow = nil;

    s_state.state = IMGUI_METAL_STATE_UNINITIALIZED;
}

bool imgui_metal_is_ready(void) {
    return s_state.state == IMGUI_METAL_STATE_READY;
}

bool imgui_metal_first_drawable_seen(void) {
    return s_first_drawable_seen;
}

ImguiMetalState imgui_metal_get_state(void) {
    return s_state.state;
}

void imgui_metal_set_visible(bool visible) {
    if (visible && s_state.state != IMGUI_METAL_STATE_READY) {
        // Store pending visibility - will be applied when ready
        s_state.pending_visible = true;
        LOG_IMGUI_INFO("Overlay visibility pending (will show when ready, state=%d)", s_state.state);
        return;
    }

    s_state.visible = visible;
    s_state.pending_visible = false;  // Clear pending flag
    LOG_IMGUI_DEBUG("Overlay visibility: %s", visible ? "shown" : "hidden");
}

bool imgui_metal_is_visible(void) {
    return s_state.visible;
}

void imgui_metal_set_input_capture(bool capture) {
    s_state.capturing_input = capture;
}

bool imgui_metal_is_capturing_input(void) {
    // True when the overlay is capturing OR a mod window is visible (so mod
    // text fields receive character input). Preserves overlay semantics when no
    // mod window is up.
    return imgui_input_target_present() && imgui_input_capture_active();
}

void imgui_metal_get_viewport_size(float *width, float *height) {
    if (s_state.state == IMGUI_METAL_STATE_READY) {
        ImGuiIO& io = ImGui::GetIO();
        if (io.DisplaySize.x > 0 && io.DisplaySize.y > 0) {
            *width = io.DisplaySize.x;
            *height = io.DisplaySize.y;
            return;
        }
    }
    *width = 0.0f;
    *height = 0.0f;
}

// ============================================================================
// Input Processing
// ============================================================================

// macOS virtual keycode to ImGui key mapping
static ImGuiKey macos_keycode_to_imgui(uint16_t keycode) {
    switch (keycode) {
        case 0x00: return ImGuiKey_A;
        case 0x01: return ImGuiKey_S;
        case 0x02: return ImGuiKey_D;
        case 0x03: return ImGuiKey_F;
        case 0x04: return ImGuiKey_H;
        case 0x05: return ImGuiKey_G;
        case 0x06: return ImGuiKey_Z;
        case 0x07: return ImGuiKey_X;
        case 0x08: return ImGuiKey_C;
        case 0x09: return ImGuiKey_V;
        case 0x0B: return ImGuiKey_B;
        case 0x0C: return ImGuiKey_Q;
        case 0x0D: return ImGuiKey_W;
        case 0x0E: return ImGuiKey_E;
        case 0x0F: return ImGuiKey_R;
        case 0x10: return ImGuiKey_Y;
        case 0x11: return ImGuiKey_T;
        case 0x12: return ImGuiKey_1;
        case 0x13: return ImGuiKey_2;
        case 0x14: return ImGuiKey_3;
        case 0x15: return ImGuiKey_4;
        case 0x16: return ImGuiKey_6;
        case 0x17: return ImGuiKey_5;
        case 0x18: return ImGuiKey_Equal;
        case 0x19: return ImGuiKey_9;
        case 0x1A: return ImGuiKey_7;
        case 0x1B: return ImGuiKey_Minus;
        case 0x1C: return ImGuiKey_8;
        case 0x1D: return ImGuiKey_0;
        case 0x1E: return ImGuiKey_RightBracket;
        case 0x1F: return ImGuiKey_O;
        case 0x20: return ImGuiKey_U;
        case 0x21: return ImGuiKey_LeftBracket;
        case 0x22: return ImGuiKey_I;
        case 0x23: return ImGuiKey_P;
        case 0x24: return ImGuiKey_Enter;
        case 0x25: return ImGuiKey_L;
        case 0x26: return ImGuiKey_J;
        case 0x27: return ImGuiKey_Apostrophe;
        case 0x28: return ImGuiKey_K;
        case 0x29: return ImGuiKey_Semicolon;
        case 0x2A: return ImGuiKey_Backslash;
        case 0x2B: return ImGuiKey_Comma;
        case 0x2C: return ImGuiKey_Slash;
        case 0x2D: return ImGuiKey_N;
        case 0x2E: return ImGuiKey_M;
        case 0x2F: return ImGuiKey_Period;
        case 0x30: return ImGuiKey_Tab;
        case 0x31: return ImGuiKey_Space;
        case 0x32: return ImGuiKey_GraveAccent;
        case 0x33: return ImGuiKey_Backspace;
        case 0x35: return ImGuiKey_Escape;
        case 0x37: return ImGuiKey_LeftSuper;  // Command
        case 0x38: return ImGuiKey_LeftShift;
        case 0x39: return ImGuiKey_CapsLock;
        case 0x3A: return ImGuiKey_LeftAlt;    // Option
        case 0x3B: return ImGuiKey_LeftCtrl;
        case 0x3C: return ImGuiKey_RightShift;
        case 0x3D: return ImGuiKey_RightAlt;
        case 0x3E: return ImGuiKey_RightCtrl;
        case 0x7A: return ImGuiKey_F1;
        case 0x78: return ImGuiKey_F2;
        case 0x63: return ImGuiKey_F3;
        case 0x76: return ImGuiKey_F4;
        case 0x60: return ImGuiKey_F5;
        case 0x61: return ImGuiKey_F6;
        case 0x62: return ImGuiKey_F7;
        case 0x64: return ImGuiKey_F8;
        case 0x65: return ImGuiKey_F9;
        case 0x6D: return ImGuiKey_F10;
        case 0x67: return ImGuiKey_F11;
        case 0x6F: return ImGuiKey_F12;
        case 0x7B: return ImGuiKey_LeftArrow;
        case 0x7C: return ImGuiKey_RightArrow;
        case 0x7D: return ImGuiKey_DownArrow;
        case 0x7E: return ImGuiKey_UpArrow;
        case 0x73: return ImGuiKey_Home;
        case 0x77: return ImGuiKey_End;
        case 0x74: return ImGuiKey_PageUp;
        case 0x79: return ImGuiKey_PageDown;
        case 0x75: return ImGuiKey_Delete;
        default: return ImGuiKey_None;
    }
}

// Debounce for F11 toggle (prevents double-toggle from fn+F11)
static uint64_t s_last_f11_toggle = 0;

bool imgui_metal_process_key(uint16_t keycode, bool down, uint32_t modifiers) {

    // F11 toggles overlay (F9/F10 conflict with game hotkeys)
    if (keycode == 0x67 && down) {  // F11
        // Debounce: ignore if toggled within last 200ms
        uint64_t now = mach_absolute_time();
        static mach_timebase_info_data_t timebase = {0, 0};
        if (timebase.denom == 0) {
            mach_timebase_info(&timebase);
        }
        uint64_t elapsed_ns = (now - s_last_f11_toggle) * timebase.numer / timebase.denom;
        uint64_t elapsed_ms = elapsed_ns / 1000000;

        if (elapsed_ms < 200) {
            LOG_IMGUI_DEBUG("F11 debounced (elapsed=%llums)", elapsed_ms);
            return true;  // Consume but don't toggle
        }

        s_last_f11_toggle = now;

        // Initialize if not yet started
        if (s_state.state == IMGUI_METAL_STATE_UNINITIALIZED) {
            LOG_IMGUI_INFO("F11 pressed - initializing ImGui Metal backend...");
            imgui_metal_init();
            // Set pending visibility so it shows when ready
            s_state.pending_visible = true;
            return true;  // Always consume F11
        }

        // If still initializing, just wait
        if (s_state.state == IMGUI_METAL_STATE_WAITING_FOR_DEVICE ||
            s_state.state == IMGUI_METAL_STATE_INITIALIZING) {
            LOG_IMGUI_INFO("F11 pressed - ImGui still initializing (state=%d)", s_state.state);
            s_state.pending_visible = true;
            return true;  // Always consume F11
        }

        // Ready - toggle visibility
        if (s_state.state == IMGUI_METAL_STATE_READY) {
            imgui_metal_set_visible(!s_state.visible);
            LOG_IMGUI_INFO("F11 toggled overlay: %s", s_state.visible ? "shown" : "hidden");
        }

        return true;  // Always consume F11
    }

    // Other keys only processed when visible
    if (!imgui_input_target_present()) {
        return false;
    }

    if (!imgui_input_capture_active()) {
        return false;
    }

    ImGuiIO& io = ImGui::GetIO();
    ImGuiKey key = macos_keycode_to_imgui(keycode);
    if (key != ImGuiKey_None) {
        io.AddKeyEvent(key, down);
    }

    // Update modifier keys
    io.AddKeyEvent(ImGuiMod_Ctrl, (modifiers & (1 << 18)) != 0);   // controlKey
    io.AddKeyEvent(ImGuiMod_Shift, (modifiers & (1 << 17)) != 0);  // shiftKey
    io.AddKeyEvent(ImGuiMod_Alt, (modifiers & (1 << 19)) != 0);    // optionKey
    io.AddKeyEvent(ImGuiMod_Super, (modifiers & (1 << 20)) != 0);  // commandKey

    return io.WantCaptureKeyboard;
}

// Convert screen coordinates to window-relative coordinates for ImGui
// CGEventGetLocation returns Quartz coordinates (origin at TOP-LEFT of main display) in POINTS
// This matches the approach used by the official ImGui OSX backend
static bool convert_screen_to_window(float screenX, float screenY, float *outX, float *outY) {
    if (!s_state.gameWindow) {
        return false;
    }

    NSView *contentView = [s_state.gameWindow contentView];
    if (!contentView) {
        return false;
    }

    // CGEventGetLocation gives Quartz coords (origin top-left of display) in POINTS
    // Step 1: Convert from CG (top-left origin) to Cocoa screen coords (bottom-left origin)
    CGFloat mainScreenHeight = [[NSScreen mainScreen] frame].size.height;
    float cocoaScreenY = mainScreenHeight - screenY;
    NSPoint screenPoint = NSMakePoint(screenX, cocoaScreenY);

    // Step 2: Convert from screen coords to window coords
    NSPoint windowPoint = [s_state.gameWindow convertPointFromScreen:screenPoint];

    // Step 3: Convert from window coords to view coords
    windowPoint = [contentView convertPoint:windowPoint fromView:nil];

    // Step 4: Flip Y for ImGui (top-left origin) if view uses bottom-left origin
    if (![contentView isFlipped]) {
        windowPoint.y = contentView.bounds.size.height - windowPoint.y;
    }

    // Debug logging (every 120th conversion to see ongoing behavior)
    static int debugCount = 0;
    debugCount++;
    if (debugCount <= 10 || debugCount % 120 == 0) {
        NSRect windowFrame = [s_state.gameWindow frame];
        LOG_IMGUI_DEBUG("CoordConvert: CG(%.0f,%.0f) -> Cocoa(%.0f,%.0f) -> View(%.0f,%.0f) [winOrigin=(%.0f,%.0f), winSize=%.0fx%.0f, viewSize=%.0fx%.0f]",
            screenX, screenY, screenPoint.x, screenPoint.y, windowPoint.x, windowPoint.y,
            windowFrame.origin.x, windowFrame.origin.y,
            windowFrame.size.width, windowFrame.size.height,
            contentView.bounds.size.width, contentView.bounds.size.height);
    }

    *outX = windowPoint.x;
    *outY = windowPoint.y;
    return true;
}

bool imgui_metal_process_mouse(float x, float y, int button, bool down) {
    if (!imgui_input_target_present()) {
        return false;
    }

    ImGuiIO& io = ImGui::GetIO();

    // Convert screen coords to window coords and update position
    float windowX = 0, windowY = 0;
    if (convert_screen_to_window(x, y, &windowX, &windowY)) {
        io.AddMousePosEvent(windowX, windowY);
    }

    if (button >= 0 && button < 5) {
        io.AddMouseButtonEvent(button, down);
        LOG_IMGUI_DEBUG("Mouse %s button %d at (%.0f,%.0f) WantCapture=%d",
            down ? "DOWN" : "UP", button, windowX, windowY, io.WantCaptureMouse);
    }

    return imgui_input_capture_active() && io.WantCaptureMouse;
}

void imgui_metal_process_mouse_move(float x, float y) {
    if (s_state.state != IMGUI_METAL_STATE_READY) {
        return;
    }

    // Convert screen coords to window coords
    float windowX, windowY;
    if (!convert_screen_to_window(x, y, &windowX, &windowY)) {
        return;
    }

    // Cache the position - will be re-applied after ImGui_ImplOSX_NewFrame
    s_cgevent_mouse.x = windowX;
    s_cgevent_mouse.y = windowY;
    s_cgevent_mouse.valid = true;

    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(windowX, windowY);

    // Log every 60th move to avoid spam
    static int moveCount = 0;
    if (++moveCount % 60 == 0) {
        LOG_IMGUI_DEBUG("MouseMove: CG(%.0f,%.0f) -> View(%.0f,%.0f)",
            x, y, windowX, windowY);
    }
}

// Direct input functions - coordinates already in ImGui space (from NSView swizzling)
bool imgui_metal_process_mouse_direct(float x, float y, int button, bool down) {
    if (!imgui_input_target_present()) {
        return false;
    }

    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);

    if (button >= 0 && button < 5) {
        io.AddMouseButtonEvent(button, down);
        LOG_IMGUI_DEBUG("Mouse %s button %d at direct(%.0f,%.0f), WantCapture=%d",
            down ? "DOWN" : "UP", button, x, y, io.WantCaptureMouse);
    }

    return imgui_input_capture_active() && io.WantCaptureMouse;
}

void imgui_metal_process_mouse_move_direct(float x, float y) {
    if (s_state.state != IMGUI_METAL_STATE_READY) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
}

void imgui_metal_process_scroll(float dx, float dy) {
    if (!imgui_input_target_present()) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    io.AddMouseWheelEvent(dx, dy);
}

void imgui_metal_process_char(unsigned int c) {
    if (!imgui_input_target_present() || !imgui_input_capture_active()) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    io.AddInputCharacter(c);
}
