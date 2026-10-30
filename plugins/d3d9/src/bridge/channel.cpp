#include "bridge/channel.h"

#include <algorithm>
#include <cstring>
#include <new>

namespace openremix::d3d9::bridge {
namespace {

// A waiting side spins this long before it blocks (Remix Plus's bridge: a
// blocked wake costs tens of microseconds, most waits end sooner).
constexpr double kSpinMicroseconds = 100;
// A blocked wait wakes this often to look at the peer.
constexpr DWORD kWaitSlice = 100;

double nowMicroseconds() {
  static const double perTick = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return 1e6 / double(f.QuadPart);
  }();
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return double(t.QuadPart) * perTick;
}

uint64_t headerBytes() { return (sizeof(SharedState) + 63) & ~uint64_t(63); }

} // namespace

std::wstring channelName(DWORD clientProcess) {
  return L"Local\\OpenRemixD3D9-" + std::to_wstring(clientProcess) + L"-" + std::to_wstring(GetTickCount64());
}

Channel::~Channel() {
  if (m_state)
    UnmapViewOfFile(m_state);
  for (HANDLE h : {m_mapping, m_data, m_space, m_frames})
    if (h)
      CloseHandle(h);
}

bool Channel::fail(const std::string &error) {
  if (!m_broken)
    m_error = error;
  m_broken = true;
  return false;
}

bool Channel::create(const std::wstring &name, uint64_t ringSize, uint32_t framesAhead) {
  const uint64_t total = headerBytes() + ringSize;
  m_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, DWORD(total >> 32), DWORD(total),
                                 name.c_str());
  if (!m_mapping)
    return fail("CreateFileMapping failed (" + std::to_string(GetLastError()) + ")");
  void *view = MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, size_t(total));
  if (!view)
    return fail("MapViewOfFile failed (" + std::to_string(GetLastError()) + ")");
  m_state = new (view) SharedState();
  m_state->magic = kMagic;
  m_state->version = kVersion;
  m_state->ringSize = ringSize;
  m_ring = static_cast<uint8_t *>(view) + headerBytes();
  m_size = ringSize;
  m_data = CreateEventW(nullptr, FALSE, FALSE, (name + L"-data").c_str());
  m_space = CreateEventW(nullptr, FALSE, FALSE, (name + L"-space").c_str());
  m_frames = CreateSemaphoreW(nullptr, LONG(std::max(framesAhead, 1u)), 1 << 20, (name + L"-frames").c_str());
  if (!m_data || !m_space || !m_frames)
    return fail("cannot create the bridge's events (" + std::to_string(GetLastError()) + ")");
  return true;
}

bool Channel::open(const std::wstring &name) {
  m_mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
  if (!m_mapping)
    return fail("OpenFileMapping failed (" + std::to_string(GetLastError()) + ")");
  void *view = MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
  if (!view)
    return fail("MapViewOfFile failed (" + std::to_string(GetLastError()) + ")");
  m_state = static_cast<SharedState *>(view);
  if (m_state->magic != kMagic || m_state->version != kVersion)
    return fail("the plugin and OpenRemixHost.exe are different versions");
  m_ring = static_cast<uint8_t *>(view) + headerBytes();
  m_size = m_state->ringSize;
  const DWORD access = SYNCHRONIZE | EVENT_MODIFY_STATE;
  m_data = OpenEventW(access, FALSE, (name + L"-data").c_str());
  m_space = OpenEventW(access, FALSE, (name + L"-space").c_str());
  m_frames = OpenSemaphoreW(SYNCHRONIZE | SEMAPHORE_MODIFY_STATE, FALSE, (name + L"-frames").c_str());
  if (!m_data || !m_space || !m_frames)
    return fail("cannot open the bridge's events (" + std::to_string(GetLastError()) + ")");
  return true;
}

bool Channel::wait(HANDLE event, DWORD milliseconds, bool &timedOut) {
  timedOut = false;
  HANDLE handles[2] = {event, m_peer};
  const DWORD count = m_peer ? 2 : 1;
  const ULONGLONG until = GetTickCount64() + milliseconds;
  for (;;) {
    const ULONGLONG now = GetTickCount64();
    const DWORD slice = milliseconds == INFINITE ? kWaitSlice
                                                 : DWORD(std::min<ULONGLONG>(now < until ? until - now : 0, kWaitSlice));
    const DWORD result = WaitForMultipleObjects(count, handles, FALSE, slice);
    if (result == WAIT_OBJECT_0)
      return true;
    if (result == WAIT_OBJECT_0 + 1)
      return fail("the other process exited");
    if (result == WAIT_FAILED)
      return fail("waiting failed (" + std::to_string(GetLastError()) + ")");
    if (milliseconds != INFINITE && GetTickCount64() >= until) {
      timedOut = true;
      return false;
    }
  }
}

bool Channel::write(const void *data, size_t size) {
  const uint8_t *bytes = static_cast<const uint8_t *>(data);
  while (size) {
    uint64_t written = m_state->written.load(std::memory_order_relaxed);
    uint64_t free = m_size - (written - m_state->read.load(std::memory_order_acquire));
    if (!free) {
      // Spin, then sleep until the host frees space.
      const double start = nowMicroseconds();
      while (!free && nowMicroseconds() - start < kSpinMicroseconds) {
        YieldProcessor();
        free = m_size - (written - m_state->read.load(std::memory_order_acquire));
      }
      while (!free) {
        m_state->writerWaiting.store(1, std::memory_order_seq_cst);
        free = m_size - (written - m_state->read.load(std::memory_order_seq_cst));
        bool timedOut;
        if (!free && !wait(m_space, kWaitSlice, timedOut) && !timedOut)
          return false;
        m_state->writerWaiting.store(0, std::memory_order_relaxed);
        free = m_size - (written - m_state->read.load(std::memory_order_acquire));
      }
    }
    const uint64_t at = written % m_size;
    const size_t chunk = size_t(std::min<uint64_t>({uint64_t(size), free, m_size - at}));
    std::memcpy(m_ring + at, bytes, chunk);
    m_state->written.store(written + chunk, std::memory_order_seq_cst);
    if (m_state->readerWaiting.load(std::memory_order_seq_cst))
      SetEvent(m_data);
    bytes += chunk;
    size -= chunk;
  }
  return true;
}

bool Channel::send(uint16_t command, std::initializer_list<Part> parts) {
  std::lock_guard<std::mutex> lock(m_sendLock);
  if (m_broken)
    return false;
  if (m_state->hostState.load(std::memory_order_acquire) >= kHostFailed)
    return fail("OpenRemixHost.exe stopped");
  PacketHeader header = {kMagic, command, 0, 0, ++m_sequence};
  size_t size = 0;
  for (const Part &part : parts)
    size += part.size;
  header.size = uint32_t(size);
  if (!write(&header, sizeof(header)))
    return false;
  for (const Part &part : parts)
    if (part.size && !write(part.data, part.size))
      return false;
  return true;
}

bool Channel::readBytes(void *data, size_t size, DWORD timeoutMs, bool &timedOut) {
  uint8_t *bytes = static_cast<uint8_t *>(data);
  timedOut = false;
  while (size) {
    const uint64_t read = m_state->read.load(std::memory_order_relaxed);
    uint64_t available = m_state->written.load(std::memory_order_acquire) - read;
    if (!available) {
      const double start = nowMicroseconds();
      while (!available && nowMicroseconds() - start < kSpinMicroseconds) {
        YieldProcessor();
        available = m_state->written.load(std::memory_order_acquire) - read;
      }
      while (!available) {
        m_state->readerWaiting.store(1, std::memory_order_seq_cst);
        available = m_state->written.load(std::memory_order_seq_cst) - read;
        if (!available && !wait(m_data, timeoutMs, timedOut)) {
          m_state->readerWaiting.store(0, std::memory_order_relaxed);
          return false;
        }
        m_state->readerWaiting.store(0, std::memory_order_relaxed);
        available = m_state->written.load(std::memory_order_acquire) - read;
      }
    }
    const uint64_t at = read % m_size;
    const size_t chunk = size_t(std::min<uint64_t>({uint64_t(size), available, m_size - at}));
    std::memcpy(bytes, m_ring + at, chunk);
    m_state->read.store(read + chunk, std::memory_order_seq_cst);
    if (m_state->writerWaiting.load(std::memory_order_seq_cst))
      SetEvent(m_space);
    bytes += chunk;
    size -= chunk;
    // A packet once begun is read to its end.
    timeoutMs = INFINITE;
  }
  return true;
}

Channel::Received Channel::receive(PacketHeader &header, std::vector<uint8_t> &payload, DWORD timeoutMs) {
  bool timedOut;
  if (!readBytes(&header, sizeof(header), timeoutMs, timedOut))
    return timedOut ? Received::Timeout : Received::Broken;
  if (header.magic != kMagic || header.sequence != ++m_sequence) {
    fail("bridge desync: packet " + std::to_string(header.sequence) + " (command " + std::to_string(header.command) +
         ") where " + std::to_string(m_sequence) + " was due");
    return Received::Broken;
  }
  payload.resize(header.size);
  if (header.size && !readBytes(payload.data(), header.size, INFINITE, timedOut))
    return Received::Broken;
  return Received::Packet;
}

bool Channel::waitFrame() {
  bool timedOut;
  // The host returns a slot per frame it presents; it may be starting up.
  while (!wait(m_frames, kWaitSlice, timedOut)) {
    if (!timedOut || m_state->hostState.load(std::memory_order_acquire) >= kHostFailed)
      return fail(m_error.empty() ? "OpenRemixHost.exe stopped" : m_error);
  }
  return true;
}

void Channel::frameDone() { ReleaseSemaphore(m_frames, 1, nullptr); }

} // namespace openremix::d3d9::bridge
