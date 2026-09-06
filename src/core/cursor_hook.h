#pragma once
#include <Windows.h>

namespace S3FC {

// CURSOR HOOK - Finds the game's own top-level window and subclasses its
// WndProc to swap in TS3.exe's own busy cursor whenever FreezeWatchdog
// reports a sim stall. This is the entire visible feature of this mod - no
// on-screen indicator, no dialog.
//
// Window discovery uses EnumWindows + PID matching rather than pulling the
// HWND out of a D3D9 device's presentation parameters (S3MM's approach in
// imgui_manager.cpp) - this mod never creates a D3D9 device of its own, by
// design, so it has no swapchain to ask. A Sims 3 process only ever has one
// top-level, visible, unowned window, so the first EnumWindows match (which
// walks in Z-order, topmost first) is always the right one.
//
// WndProc subclassing uses the same SetWindowLongPtr/CallWindowProc chain
// pattern S3MM uses. That pattern is specifically designed to compose: if
// another mod (e.g. S3MM) also subclasses the same window, whichever
// subclasses SECOND naturally becomes the outer layer and calls through to
// the other via CallWindowProc, regardless of install order - so this mod
// and S3MM's own copy of this feature can coexist on the same window
// without coordinating.
class CursorHook {
  public:
    static CursorHook& Get();

    // Attempts to find the game's main window and subclass it. Safe to call
    // repeatedly - a no-op once installed, and cheap to retry while the
    // window doesn't exist yet (see freeze_watchdog.cpp's PollLoop, which
    // retries this every 150ms until it succeeds).
    bool Initialize();
    bool IsInstalled() const { return m_installed; }

    // Diagnostic counters for the watchdog's periodic stats line. "Not
    // seeing the busy cursor" is not evidence the cursor path is idle:
    // WM_SETCURSOR only arrives while the pointer is moving over the
    // window, and a stall short enough to miss visually still runs every
    // line of this hook. These say whether it actually ran.
    static long SetCursorMessages();
    static long CursorSwaps();

  private:
    CursorHook() = default;
    ~CursorHook() = default;
    CursorHook(const CursorHook&) = delete;
    CursorHook& operator=(const CursorHook&) = delete;

    static LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    static volatile LONG s_setCursorMessages; // WM_SETCURSOR seen by the subclass
    static volatile LONG s_cursorSwaps;       // SetCursor() calls actually made

    bool m_installed = false;
    HWND m_hwnd = nullptr;
    WNDPROC m_originalWndProc = nullptr;
};

} // namespace S3FC
