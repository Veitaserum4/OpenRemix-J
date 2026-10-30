#pragma once
#include <windows.h>

#include <cstdint>

// The game window's input, forwarded by the 32-bit plugin, delivered to
// OpenRemix's window in OpenRemixHost.exe as SDL events. Posting the window
// messages to OpenRemix's stand-in window is not enough: SDL gives keyboard
// focus only to the foreground window, which the stand-in never is, and
// drops key events without it.
namespace openremix::d3d9::bridge {

class HostInput {
public:
  // A forwarded message (WM_KEY*, WM_CHAR, mouse, focus). Ignored until
  // OpenRemix has made its window.
  void deliver(UINT message, WPARAM w, LPARAM l);

private:
  bool attach();
  void key(UINT message, WPARAM w, LPARAM l);
  void text(WPARAM w);
  void mouse(UINT message, WPARAM w, LPARAM l);

  bool m_attached = false;
  uint32_t m_window = 0;   // OpenRemix's SDL window id
  uint32_t m_modifiers = 0; // SDL_Keymod from the forwarded keys
  uint32_t m_buttons = 0;   // SDL mouse button mask
  float m_x = 0, m_y = 0;
  wchar_t m_highSurrogate = 0;
};

} // namespace openremix::d3d9::bridge
