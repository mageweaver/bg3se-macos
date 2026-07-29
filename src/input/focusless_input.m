/**
 * focusless_input.m - In-process input injection without focus
 *
 * Calls [LSMTLView keyDown:] / [LSMTLView keyUp:] directly on BG3's
 * Metal view.  This bypasses AppKit's event routing entirely — the
 * view's keyDown: implementation calls ls::InputManager::InjectInput()
 * regardless of whether the app is frontmost.
 *
 * Discovery: Ghidra RE of LSMTLView::keyDown_ (0x100bd798c) shows it
 * reads the InputManager* from ivar at offset 104, translates the
 * macOS keyCode via s_KeyboardKeys[], and calls InjectInput directly.
 */

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#include <dispatch/dispatch.h>
#include <os/log.h>
#include <objc/runtime.h>

#include "focusless_input.h"
#include "../core/logging.h"
#include "../game/game_state.h"
#include "../imgui/imgui_metal_backend.h"

static bool s_initialized = false;
static dispatch_source_t s_splash_timer = NULL;
static bool s_socket_ready = false;
static int s_dismiss_count = 0;
static int s_max_dismiss = 0;

bool focusless_input_init(void) {
    if (s_initialized) return true;
    s_initialized = true;
    s_socket_ready = false;
    s_dismiss_count = 0;
    LOG_CORE_INFO("[FocuslessInput] Initialized");
    return true;
}

void focusless_input_shutdown(void) {
    focusless_input_mark_socket_ready();
    s_initialized = false;
    LOG_CORE_INFO("[FocuslessInput] Shutdown");
}

static NSString *chars_for_keycode(uint16_t keyCode) {
    switch (keyCode) {
        case 0x24: return @"\r";   // kVK_Return
        case 0x31: return @" ";    // kVK_Space
        case 0x35: return @"\x1b"; // kVK_Escape
        case 0x33: return @"\x7f"; // kVK_Delete (backspace)
        case 0x30: return @"\t";   // kVK_Tab
        default:   return @" ";
    }
}

static NSView *find_lsmtlview(void) {
    Class lsmtlClass = objc_getClass("LSMTLView");
    if (!lsmtlClass) {
        LOG_CORE_DEBUG("[FocuslessInput] LSMTLView class not found");
        return nil;
    }

    for (NSWindow *w in [NSApp windows]) {
        NSView *cv = [w contentView];
        if ([cv isKindOfClass:lsmtlClass]) return cv;
        for (NSView *sub in [cv subviews]) {
            if ([sub isKindOfClass:lsmtlClass]) return sub;
        }
    }
    return nil;
}

static bool try_direct_view_key(uint16_t keyCode, uint32_t modifiers) {
    NSView *view = find_lsmtlview();
    if (!view) return false;

    // Check if InputManager ivar is set (offset 104 per Ghidra RE)
    void *inputMgr = NULL;
    Ivar ivar = class_getInstanceVariable([view class], "inputManager");
    if (ivar) {
        inputMgr = *(void **)((uint8_t *)(__bridge void *)view + ivar_getOffset(ivar));
    }
    if (!inputMgr) {
        LOG_CORE_DEBUG("[FocuslessInput] LSMTLView found but inputManager is NULL (ivar=%p)", (void *)ivar);
        return false;
    }

    NSEventModifierFlags nsFlags = 0;
    if (modifiers & (1 << 0)) nsFlags |= NSEventModifierFlagShift;
    if (modifiers & (1 << 1)) nsFlags |= NSEventModifierFlagControl;
    if (modifiers & (1 << 2)) nsFlags |= NSEventModifierFlagOption;
    if (modifiers & (1 << 3)) nsFlags |= NSEventModifierFlagCommand;

    NSString *chars = chars_for_keycode(keyCode);
    NSWindow *win = [view window];
    NSInteger winNum = win ? [win windowNumber] : 0;

    NSEvent *down = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                     location:NSMakePoint(0, 0)
                                modifierFlags:nsFlags
                                    timestamp:[[NSProcessInfo processInfo] systemUptime]
                                 windowNumber:winNum
                                      context:nil
                                   characters:chars
                  charactersIgnoringModifiers:chars
                                    isARepeat:NO
                                      keyCode:keyCode];

    NSEvent *up = [NSEvent keyEventWithType:NSEventTypeKeyUp
                                   location:NSMakePoint(0, 0)
                              modifierFlags:nsFlags
                                  timestamp:[[NSProcessInfo processInfo] systemUptime] + 0.05
                               windowNumber:winNum
                                    context:nil
                                 characters:chars
                charactersIgnoringModifiers:chars
                                  isARepeat:NO
                                    keyCode:keyCode];

    LOG_CORE_DEBUG("[FocuslessInput] Calling [LSMTLView keyDown:] key=%d inputMgr=%p win=%ld",
                  keyCode, inputMgr, (long)winNum);
    [view keyDown:down];
    [view keyUp:up];
    return true;
}

static bool try_direct_view_mouse_click(double x_fraction, double y_fraction_top_origin) {
    NSView *view = find_lsmtlview();
    if (!view) return false;

    void *inputMgr = NULL;
    Ivar ivar = class_getInstanceVariable([view class], "inputManager");
    if (ivar) {
        inputMgr = *(void **)((uint8_t *)(__bridge void *)view + ivar_getOffset(ivar));
    }
    if (!inputMgr) {
        LOG_CORE_DEBUG("[FocuslessInput] LSMTLView found but inputManager is NULL for mouse (ivar=%p)", (void *)ivar);
        return false;
    }

    NSWindow *win = [view window];
    NSInteger winNum = win ? [win windowNumber] : 0;
    NSRect bounds = [view bounds];
    CGFloat x = NSMinX(bounds) + NSWidth(bounds) * x_fraction;
    CGFloat y = NSMinY(bounds) + NSHeight(bounds) * (1.0 - y_fraction_top_origin);
    NSPoint location = NSMakePoint(x, y);
    NSTimeInterval now = [[NSProcessInfo processInfo] systemUptime];

    NSEvent *down = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
                                       location:location
                                  modifierFlags:0
                                      timestamp:now
                                   windowNumber:winNum
                                        context:nil
                                    eventNumber:0
                                     clickCount:1
                                       pressure:1.0];

    NSEvent *up = [NSEvent mouseEventWithType:NSEventTypeLeftMouseUp
                                     location:location
                                modifierFlags:0
                                    timestamp:now + 0.08
                                 windowNumber:winNum
                                      context:nil
                                  eventNumber:0
                                   clickCount:1
                                     pressure:0.0];

    LOG_CORE_DEBUG("[FocuslessInput] Calling [LSMTLView mouseDown:] x=%.1f y=%.1f xf=%.3f yf=%.3f inputMgr=%p win=%ld",
                  x, y, x_fraction, y_fraction_top_origin, inputMgr, (long)winNum);
    [view mouseMoved:down];
    [view mouseDown:down];
    [view mouseUp:up];
    return true;
}

bool focusless_input_post_key_press(uint16_t keyCode, uint32_t modifiers) {
    if (!s_initialized) return false;

    dispatch_async(dispatch_get_main_queue(), ^{
        @autoreleasepool {
            bool ok = try_direct_view_key(keyCode, modifiers);
            LOG_CORE_DEBUG("[FocuslessInput] Posted key %d (direct_view=%s, attempt=%d)",
                          keyCode, ok ? "yes" : "no", s_dismiss_count);
        }
    });

    return true;
}

bool focusless_input_post_mouse_click(double x_fraction, double y_fraction_top_origin) {
    if (!s_initialized) return false;

    dispatch_async(dispatch_get_main_queue(), ^{
        @autoreleasepool {
            bool ok = try_direct_view_mouse_click(x_fraction, y_fraction_top_origin);
            LOG_CORE_DEBUG("[FocuslessInput] Posted mouse click xf=%.3f yf=%.3f (direct_view=%s, attempt=%d)",
                          x_fraction, y_fraction_top_origin, ok ? "yes" : "no", s_dismiss_count);
        }
    });

    return true;
}

void focusless_input_start_splash_autodismiss(double duration, double interval) {
    if (!s_initialized) return;
    if (s_splash_timer) return;

    s_socket_ready = false;
    s_dismiss_count = 0;
    s_max_dismiss = (int)(duration / interval);

    LOG_CORE_INFO("[FocuslessInput] Splash auto-dismiss started (%.0fs duration, %.1fs interval, max %d attempts)",
                  duration, interval, s_max_dismiss);

    // Use global queue — main queue is blocked during Bink video playback
    dispatch_queue_t queue = dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0);
    s_splash_timer = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);

    uint64_t interval_ns = (uint64_t)(interval * NSEC_PER_SEC);
    uint64_t start_delay_ns = (uint64_t)(3.0 * NSEC_PER_SEC);
    dispatch_source_set_timer(s_splash_timer,
                              dispatch_time(DISPATCH_TIME_NOW, start_delay_ns),
                              interval_ns, interval_ns / 10);

    dispatch_source_set_event_handler(s_splash_timer, ^{
        // Stop once the game has actually advanced past the splash (a session
        // is loading or running) — NOT on socket traffic: the harness polls
        // !identity during boot, which previously cancelled us at 0 attempts.
        ServerGameState gs = game_state_get_current();
        bool past_splash = (gs >= SERVER_STATE_LOAD_LEVEL &&
                            gs <= SERVER_STATE_RUNNING);
        if (s_socket_ready || past_splash || s_dismiss_count >= s_max_dismiss) {
            LOG_CORE_INFO("[FocuslessInput] Splash timer stopped (%s, %d attempts)",
                          s_socket_ready ? "explicit stop"
                                         : past_splash ? "game state advanced"
                                                       : "max reached",
                          s_dismiss_count);
            dispatch_source_cancel(s_splash_timer);
            s_splash_timer = NULL;
            return;
        }

        s_dismiss_count++;
        if (imgui_metal_first_drawable_seen()) {
            LOG_CORE_INFO("[FocuslessInput] Dismiss attempt #%d (Escape+Space, Metal ready)", s_dismiss_count);
        } else {
            LOG_CORE_INFO("[FocuslessInput] Dismiss attempt #%d (Escape+Space, pre-Metal)", s_dismiss_count);
        }

        // kVK_Escape = 0x35 (skips Bink intro videos)
        focusless_input_post_key_press(0x35, 0);

        // kVK_Space = 0x31 (dismisses "Press Any Key" splash)
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(200 * NSEC_PER_MSEC)),
                       dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
            focusless_input_post_key_press(0x31, 0);
        });

        // Some BG3 pre-Metal screens ignore key events but accept mouse
        // activation through LSMTLView. Keep this native loop splash-only:
        // foreground Python watchdog owns main-menu and modal clicks. Sending
        // Escape/Return/clicks after the Mod Verification modal appears causes
        // the UI to open and close repeatedly.
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(800 * NSEC_PER_MSEC)),
                       dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
            focusless_input_post_mouse_click(0.5, 0.5);
        });
    });

    dispatch_source_set_cancel_handler(s_splash_timer, ^{
        LOG_CORE_DEBUG("[FocuslessInput] Timer cancelled");
    });

    dispatch_resume(s_splash_timer);
}

void focusless_input_mark_socket_ready(void) {
    s_socket_ready = true;
    if (s_splash_timer) {
        LOG_CORE_INFO("[FocuslessInput] Socket ready — stopping splash timer after %d attempts",
                      s_dismiss_count);
        dispatch_source_cancel(s_splash_timer);
        s_splash_timer = NULL;
    }
}
