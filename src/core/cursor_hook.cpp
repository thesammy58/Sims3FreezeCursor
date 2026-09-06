#include "cursor_hook.h"
#include "freeze_watchdog.h"
#include "../util/logger.h"
#include <Windows.h>

namespace S3FC {

namespace {

// WINDOW DISCOVERY - finds this process's own top-level, visible, unowned
// window. EnumWindows enumerates in Z-order (topmost first); a Sims 3
// process only ever has one such window, so the first match is it. See
// cursor_hook.h for why this doesn't go through D3D9 like S3MM's own copy
// of this feature does.
struct FindContext {
    DWORD pid = 0;
    HWND result = nullptr;
};

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    FindContext* ctx = reinterpret_cast<FindContext*>(lParam);

    DWORD windowPid = 0;
    GetWindowThreadProcessId(hwnd, &windowPid);
    if (windowPid != ctx->pid) return TRUE; // not ours - keep enumerating

    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE; // skip owned windows (dialogs, tooltips)

    ctx->result = hwnd;
    return FALSE; // found it - stop enumerating
}

HWND FindGameWindow() {
    FindContext ctx;
    ctx.pid = GetCurrentProcessId();
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.result;
}

// BUSY CURSOR - TS3.exe's own embedded busy-cursor asset: RT_ANICURSOR
// resource 103, a blue rotating spinner, confirmed by direct PE resource
// extraction/visual inspection to be one of the game's own cursors (see the
// project's design doc). Loaded straight from the running game process's
// own module via GetModuleHandle(nullptr) - since this mod's DLL is
// injected into the same process as TS3.exe, this has no Mono/icall
// dependency at all.
HCURSOR GetBusyCursor() {
    static HCURSOR s_busyCursor = nullptr;
    static bool s_loadAttempted = false;
    if (!s_loadAttempted) {
        s_loadAttempted = true;
        s_busyCursor = LoadCursor(GetModuleHandle(nullptr), MAKEINTRESOURCE(103));
        if (s_busyCursor) {
            LOG_INFO("[CursorHook] Loaded busy cursor (game executable resource 103)");
        } else {
            LOG_WARN("[CursorHook] Busy cursor (resource 103) failed to load, error " + ToHex(GetLastError()) + " - stalls will show no cursor change.");
        }
    }
    return s_busyCursor;
}

} // namespace

volatile LONG CursorHook::s_setCursorMessages = 0;
volatile LONG CursorHook::s_cursorSwaps = 0;

long CursorHook::SetCursorMessages() { return InterlockedCompareExchange(&s_setCursorMessages, 0, 0); }
long CursorHook::CursorSwaps() { return InterlockedCompareExchange(&s_cursorSwaps, 0, 0); }

CursorHook& CursorHook::Get() {
    static CursorHook instance;
    return instance;
}

bool CursorHook::Initialize() {
    if (m_installed) return true;

    m_hwnd = FindGameWindow();
    if (!m_hwnd) return false; // window doesn't exist yet - caller retries

    m_originalWndProc = (WNDPROC)SetWindowLongPtr(m_hwnd, GWLP_WNDPROC, (LONG_PTR)HookedWndProc);
    if (!m_originalWndProc) {
        LOG_ERROR("[CursorHook] SetWindowLongPtr failed, GetLastError=" + std::to_string(GetLastError()));
        m_hwnd = nullptr;
        return false;
    }

    m_installed = true;
    LOG_INFO("[CursorHook] Subclassed game window " + ToHex((uintptr_t)m_hwnd));
    return true;
}

LRESULT CALLBACK CursorHook::HookedWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    // Edge-triggered application flag, ported from S3MM's own
    // imgui_manager.cpp: Win32 restarts an animated (.ani) cursor's
    // playback from frame 0 every time SetCursor() is called, even with the
    // same handle - so this is only set true once per stall, on the
    // transition into it, not on every WM_SETCURSOR (which fires
    // constantly, at minimum on every mouse move). That earlier bug (in
    // S3MM) made the spinner visibly stutter/restart instead of playing
    // smoothly.
    static bool s_cursorAppliedThisStall = false;

    if (uMsg == WM_SETCURSOR) {
        InterlockedIncrement(&s_setCursorMessages);
        bool stalled = (FreezeWatchdog::Get().GetState() == WatchdogState::Stalled);
        HCURSOR busyCursor = GetBusyCursor();

        if (stalled) {
            if (busyCursor) {
                if (!s_cursorAppliedThisStall) {
                    SetCursor(busyCursor);
                    InterlockedIncrement(&s_cursorSwaps);
                    s_cursorAppliedThisStall = true;
                }
                // Return TRUE on EVERY WM_SETCURSOR while stalled - not just
                // the edge - so the game's own WndProc (or another mod's
                // subclass further down the chain) never gets a chance to
                // repaint its own cursor over ours for the rest of the stall.
                return TRUE;
            }
            // Resource 103 failed to load - deliberately do NOT fall back to
            // any generic Windows cursor (IDC_APPSTARTING, IDC_WAIT, or
            // otherwise). Players must never see a plain Windows cursor for
            // a mere sim stall. Fall through instead.
        } else {
            s_cursorAppliedThisStall = false;
        }
    }

    // WM_ACTIVATE/WM_ACTIVATEAPP are inherently edge-triggered by Windows
    // itself (they only fire on an actual focus transition), so it's safe
    // to call SetCursor() here unconditionally without reintroducing the
    // animation-restart problem above. This fixes a real bug found while
    // building the S3MM version of this same feature: focus-out during a
    // stall, then focus back in, used to leave the plain Windows arrow
    // showing until the sim heartbeat resumed, because nothing re-asserted
    // the busy cursor on the way back in - vanilla TS3 restores its own
    // custom cursor immediately on refocus even while its own thread is
    // busy, so this matches that expectation instead of a jarring
    // regression from it.
    if ((uMsg == WM_ACTIVATE && LOWORD(wParam) != WA_INACTIVE) ||
        (uMsg == WM_ACTIVATEAPP && wParam != FALSE)) {
        bool stalled = (FreezeWatchdog::Get().GetState() == WatchdogState::Stalled);
        HCURSOR busyCursor = GetBusyCursor();
        if (stalled && busyCursor) {
            SetCursor(busyCursor);
            InterlockedIncrement(&s_cursorSwaps);
            s_cursorAppliedThisStall = true;
        }
        // Does NOT return - activation still needs to reach the game's own
        // WndProc (and any other mod further down the subclass chain).
    }

    return CallWindowProc(Get().m_originalWndProc, hWnd, uMsg, wParam, lParam);
}

} // namespace S3FC
