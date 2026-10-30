#include "bridge/host_input.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_video.h>
#include <windowsx.h>

namespace openremix::d3d9::bridge {
namespace {

// SDL3.dll is remix.dll's, loaded with it: called through its exports.
struct Sdl {
  decltype(&SDL_PushEvent) pushEvent = nullptr;
  decltype(&SDL_GetWindows) getWindows = nullptr;
  decltype(&SDL_GetWindowID) getWindowID = nullptr;
  decltype(&SDL_GetKeyFromScancode) keyFromScancode = nullptr;
  decltype(&SDL_free) free = nullptr;
} g_sdl;

template <class Fn> void resolve(HMODULE module, Fn &fn, const char *name) {
  fn = reinterpret_cast<Fn>(GetProcAddress(module, name));
}

// The key's SDL scancode from its virtual key, and from the scan code's
// extended bit for the keys that share one (right Ctrl and Alt, the keypad's
// Enter and the arrows block).
SDL_Scancode scancode(WPARAM vk, LPARAM l) {
  const bool extended = (HIWORD(l) & KF_EXTENDED) != 0;
  if (vk >= 'A' && vk <= 'Z')
    return SDL_Scancode(SDL_SCANCODE_A + (vk - 'A'));
  if (vk >= '1' && vk <= '9')
    return SDL_Scancode(SDL_SCANCODE_1 + (vk - '1'));
  if (vk >= VK_F1 && vk <= VK_F12)
    return SDL_Scancode(SDL_SCANCODE_F1 + (vk - VK_F1));
  if (vk >= VK_NUMPAD1 && vk <= VK_NUMPAD9)
    return SDL_Scancode(SDL_SCANCODE_KP_1 + (vk - VK_NUMPAD1));
  switch (vk) {
  case '0': return SDL_SCANCODE_0;
  case VK_NUMPAD0: return SDL_SCANCODE_KP_0;
  case VK_RETURN: return extended ? SDL_SCANCODE_KP_ENTER : SDL_SCANCODE_RETURN;
  case VK_ESCAPE: return SDL_SCANCODE_ESCAPE;
  case VK_BACK: return SDL_SCANCODE_BACKSPACE;
  case VK_TAB: return SDL_SCANCODE_TAB;
  case VK_SPACE: return SDL_SCANCODE_SPACE;
  case VK_OEM_MINUS: return SDL_SCANCODE_MINUS;
  case VK_OEM_PLUS: return SDL_SCANCODE_EQUALS;
  case VK_OEM_4: return SDL_SCANCODE_LEFTBRACKET;
  case VK_OEM_6: return SDL_SCANCODE_RIGHTBRACKET;
  case VK_OEM_5: return SDL_SCANCODE_BACKSLASH;
  case VK_OEM_1: return SDL_SCANCODE_SEMICOLON;
  case VK_OEM_7: return SDL_SCANCODE_APOSTROPHE;
  case VK_OEM_3: return SDL_SCANCODE_GRAVE;
  case VK_OEM_COMMA: return SDL_SCANCODE_COMMA;
  case VK_OEM_PERIOD: return SDL_SCANCODE_PERIOD;
  case VK_OEM_2: return SDL_SCANCODE_SLASH;
  case VK_CAPITAL: return SDL_SCANCODE_CAPSLOCK;
  case VK_INSERT: return SDL_SCANCODE_INSERT;
  case VK_DELETE: return SDL_SCANCODE_DELETE;
  case VK_HOME: return SDL_SCANCODE_HOME;
  case VK_END: return SDL_SCANCODE_END;
  case VK_PRIOR: return SDL_SCANCODE_PAGEUP;
  case VK_NEXT: return SDL_SCANCODE_PAGEDOWN;
  case VK_LEFT: return SDL_SCANCODE_LEFT;
  case VK_RIGHT: return SDL_SCANCODE_RIGHT;
  case VK_UP: return SDL_SCANCODE_UP;
  case VK_DOWN: return SDL_SCANCODE_DOWN;
  case VK_PAUSE: return SDL_SCANCODE_PAUSE;
  case VK_SNAPSHOT: return SDL_SCANCODE_PRINTSCREEN;
  case VK_ADD: return SDL_SCANCODE_KP_PLUS;
  case VK_SUBTRACT: return SDL_SCANCODE_KP_MINUS;
  case VK_MULTIPLY: return SDL_SCANCODE_KP_MULTIPLY;
  case VK_DIVIDE: return SDL_SCANCODE_KP_DIVIDE;
  case VK_DECIMAL: return SDL_SCANCODE_KP_PERIOD;
  case VK_SHIFT: return ((l >> 16) & 0xff) == 0x36 ? SDL_SCANCODE_RSHIFT : SDL_SCANCODE_LSHIFT;
  case VK_LSHIFT: return SDL_SCANCODE_LSHIFT;
  case VK_RSHIFT: return SDL_SCANCODE_RSHIFT;
  case VK_CONTROL: return extended ? SDL_SCANCODE_RCTRL : SDL_SCANCODE_LCTRL;
  case VK_LCONTROL: return SDL_SCANCODE_LCTRL;
  case VK_RCONTROL: return SDL_SCANCODE_RCTRL;
  case VK_MENU: return extended ? SDL_SCANCODE_RALT : SDL_SCANCODE_LALT;
  case VK_LMENU: return SDL_SCANCODE_LALT;
  case VK_RMENU: return SDL_SCANCODE_RALT;
  case VK_LWIN: return SDL_SCANCODE_LGUI;
  case VK_RWIN: return SDL_SCANCODE_RGUI;
  default: return SDL_SCANCODE_UNKNOWN;
  }
}

SDL_Keymod modifierOf(SDL_Scancode code) {
  switch (code) {
  case SDL_SCANCODE_LSHIFT: return SDL_KMOD_LSHIFT;
  case SDL_SCANCODE_RSHIFT: return SDL_KMOD_RSHIFT;
  case SDL_SCANCODE_LCTRL: return SDL_KMOD_LCTRL;
  case SDL_SCANCODE_RCTRL: return SDL_KMOD_RCTRL;
  case SDL_SCANCODE_LALT: return SDL_KMOD_LALT;
  case SDL_SCANCODE_RALT: return SDL_KMOD_RALT;
  case SDL_SCANCODE_LGUI: return SDL_KMOD_LGUI;
  case SDL_SCANCODE_RGUI: return SDL_KMOD_RGUI;
  default: return SDL_KMOD_NONE;
  }
}

} // namespace

bool HostInput::attach() {
  if (m_attached)
    return true;
  HMODULE sdl = GetModuleHandleW(L"SDL3.dll");
  if (!sdl)
    return false;
  resolve(sdl, g_sdl.pushEvent, "SDL_PushEvent");
  resolve(sdl, g_sdl.getWindows, "SDL_GetWindows");
  resolve(sdl, g_sdl.getWindowID, "SDL_GetWindowID");
  resolve(sdl, g_sdl.keyFromScancode, "SDL_GetKeyFromScancode");
  resolve(sdl, g_sdl.free, "SDL_free");
  if (!g_sdl.pushEvent || !g_sdl.getWindows || !g_sdl.getWindowID || !g_sdl.keyFromScancode || !g_sdl.free)
    return false;
  // The host's one window: OpenRemix's stand-in for the game's.
  int count = 0;
  SDL_Window **windows = g_sdl.getWindows(&count);
  if (windows && count > 0)
    m_window = g_sdl.getWindowID(windows[0]);
  g_sdl.free(windows);
  m_attached = m_window != 0;
  return m_attached;
}

void HostInput::key(UINT message, WPARAM w, LPARAM l) {
  const SDL_Scancode code = scancode(w, l);
  if (code == SDL_SCANCODE_UNKNOWN)
    return;
  const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
  if (const SDL_Keymod modifier = modifierOf(code))
    m_modifiers = down ? m_modifiers | modifier : m_modifiers & ~uint32_t(modifier);
  SDL_Event event = {};
  event.key.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
  event.key.windowID = m_window;
  event.key.scancode = code;
  event.key.mod = SDL_Keymod(m_modifiers);
  event.key.key = g_sdl.keyFromScancode(code, event.key.mod, true);
  event.key.raw = Uint16((l >> 16) & 0xff);
  event.key.down = down;
  // Bit 30: the key was already down (auto-repeat).
  event.key.repeat = down && (l & (1 << 30)) != 0;
  g_sdl.pushEvent(&event);
}

void HostInput::text(WPARAM w) {
  // UTF-16 code units to one code point, then UTF-8.
  const wchar_t unit = wchar_t(w);
  if (unit >= 0xd800 && unit < 0xdc00) {
    m_highSurrogate = unit;
    return;
  }
  uint32_t point = unit;
  if (unit >= 0xdc00 && unit < 0xe000) {
    if (!m_highSurrogate)
      return;
    point = 0x10000 + ((uint32_t(m_highSurrogate) - 0xd800) << 10) + (unit - 0xdc00);
  }
  m_highSurrogate = 0;
  if (point < 32 || point == 127)
    return; // control characters come as keys
  // SDL keeps the pointer until the event is handled: a ring of buffers.
  static char ring[64][5];
  static unsigned next = 0;
  char *out = ring[next++ % 64];
  if (point < 0x80) {
    out[0] = char(point), out[1] = 0;
  } else if (point < 0x800) {
    out[0] = char(0xc0 | point >> 6), out[1] = char(0x80 | (point & 0x3f)), out[2] = 0;
  } else if (point < 0x10000) {
    out[0] = char(0xe0 | point >> 12), out[1] = char(0x80 | ((point >> 6) & 0x3f)),
    out[2] = char(0x80 | (point & 0x3f)), out[3] = 0;
  } else {
    out[0] = char(0xf0 | point >> 18), out[1] = char(0x80 | ((point >> 12) & 0x3f)),
    out[2] = char(0x80 | ((point >> 6) & 0x3f)), out[3] = char(0x80 | (point & 0x3f)), out[4] = 0;
  }
  SDL_Event event = {};
  event.text.type = SDL_EVENT_TEXT_INPUT;
  event.text.windowID = m_window;
  event.text.text = out;
  g_sdl.pushEvent(&event);
}

void HostInput::mouse(UINT message, WPARAM w, LPARAM l) {
  SDL_Event event = {};
  if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) {
    // The wheel's position is in screen coordinates: the last motion's.
    const float amount = float(GET_WHEEL_DELTA_WPARAM(w)) / WHEEL_DELTA;
    event.wheel.type = SDL_EVENT_MOUSE_WHEEL;
    event.wheel.windowID = m_window;
    (message == WM_MOUSEWHEEL ? event.wheel.y : event.wheel.x) = amount;
    event.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    event.wheel.mouse_x = m_x, event.wheel.mouse_y = m_y;
    g_sdl.pushEvent(&event);
    return;
  }
  const float x = float(GET_X_LPARAM(l)), y = float(GET_Y_LPARAM(l));
  if (x != m_x || y != m_y || message == WM_MOUSEMOVE) {
    event.motion.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.windowID = m_window;
    event.motion.state = m_buttons;
    event.motion.x = x, event.motion.y = y;
    event.motion.xrel = x - m_x, event.motion.yrel = y - m_y;
    m_x = x, m_y = y;
    g_sdl.pushEvent(&event);
  }
  Uint8 button = 0;
  bool down = false;
  switch (message) {
  case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: button = SDL_BUTTON_LEFT, down = true; break;
  case WM_LBUTTONUP: button = SDL_BUTTON_LEFT; break;
  case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: button = SDL_BUTTON_RIGHT, down = true; break;
  case WM_RBUTTONUP: button = SDL_BUTTON_RIGHT; break;
  case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: button = SDL_BUTTON_MIDDLE, down = true; break;
  case WM_MBUTTONUP: button = SDL_BUTTON_MIDDLE; break;
  case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK:
    button = GET_XBUTTON_WPARAM(w) == XBUTTON1 ? SDL_BUTTON_X1 : SDL_BUTTON_X2, down = true;
    break;
  case WM_XBUTTONUP: button = GET_XBUTTON_WPARAM(w) == XBUTTON1 ? SDL_BUTTON_X1 : SDL_BUTTON_X2; break;
  default: return;
  }
  m_buttons = down ? m_buttons | SDL_BUTTON_MASK(button) : m_buttons & ~SDL_BUTTON_MASK(button);
  event = {};
  event.button.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
  event.button.windowID = m_window;
  event.button.button = button;
  event.button.down = down;
  event.button.clicks = 1;
  event.button.x = x, event.button.y = y;
  g_sdl.pushEvent(&event);
}

void HostInput::deliver(UINT message, WPARAM w, LPARAM l) {
  if (!attach())
    return;
  switch (message) {
  case WM_KEYDOWN:
  case WM_SYSKEYDOWN:
  case WM_KEYUP:
  case WM_SYSKEYUP:
    key(message, w, l);
    return;
  case WM_CHAR:
  case WM_SYSCHAR:
    if (message == WM_CHAR)
      text(w);
    return;
  case WM_SETFOCUS:
  case WM_KILLFOCUS: {
    // Losing focus releases what was held.
    if (message == WM_KILLFOCUS)
      m_modifiers = 0, m_buttons = 0;
    SDL_Event event = {};
    event.window.type = message == WM_SETFOCUS ? SDL_EVENT_WINDOW_FOCUS_GAINED : SDL_EVENT_WINDOW_FOCUS_LOST;
    event.window.windowID = m_window;
    g_sdl.pushEvent(&event);
    return;
  }
  default:
    if ((message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) || message == WM_MOUSEHWHEEL)
      mouse(message, w, l);
    return;
  }
}

} // namespace openremix::d3d9::bridge
