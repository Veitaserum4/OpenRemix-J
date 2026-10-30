#pragma once
#include "bridge/protocol.h"

#include <windows.h>

#include <initializer_list>
#include <mutex>
#include <string>
#include <vector>

// The bridge's transport: one named shared-memory ring from the client (the
// game's process) to the host, packets streamed through it in pieces (any
// size fits), and named events to wake a waiting side. A waiting side spins
// briefly before it blocks, and every wait also watches the other process, so
// neither hangs on a peer that died.
namespace openremix::d3d9::bridge {

class Channel {
public:
  ~Channel();

  // Client: creates the memory and the events.
  bool create(const std::wstring &name, uint64_t ringSize, uint32_t framesAhead);
  // Host: opens what the client created.
  bool open(const std::wstring &name);
  // The other process: waits fail once it has exited.
  void setPeer(HANDLE process) { m_peer = process; }

  bool ready() const { return m_state != nullptr; }
  SharedState &state() { return *m_state; }
  bool broken() const { return m_broken; }
  const std::string &error() const { return m_error; }

  struct Part {
    const void *data;
    size_t size;
  };
  // Client, any thread: one packet of the parts, in order. False once the
  // host is gone.
  bool send(uint16_t command, std::initializer_list<Part> parts);

  enum class Received { Packet, Timeout, Broken };
  // Host: the next packet (waiting up to `timeoutMs` for one to begin).
  Received receive(PacketHeader &header, std::vector<uint8_t> &payload, DWORD timeoutMs);

  // Present pacing: the client waits for a frame slot after each Present, the
  // host returns one when it has presented.
  bool waitFrame();
  void frameDone();

private:
  bool fail(const std::string &error);
  bool write(const void *data, size_t size);
  bool readBytes(void *data, size_t size, DWORD timeoutMs, bool &timedOut);
  // Waits for `event` (or the peer's exit); false when the peer is gone.
  bool wait(HANDLE event, DWORD milliseconds, bool &timedOut);

  HANDLE m_mapping = nullptr, m_data = nullptr, m_space = nullptr, m_frames = nullptr, m_peer = nullptr;
  SharedState *m_state = nullptr;
  uint8_t *m_ring = nullptr;
  uint64_t m_size = 0;
  std::mutex m_sendLock;
  uint32_t m_sequence = 0;
  bool m_broken = false;
  std::string m_error;
};

// The bridge's object names for a channel name.
std::wstring channelName(DWORD clientProcess);

} // namespace openremix::d3d9::bridge
