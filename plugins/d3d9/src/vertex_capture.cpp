#include "vertex_capture.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#define XXH_INLINE_ALL
#include "xxhash.h"

namespace openremix::d3d9 {
namespace {

constexpr DWORD kEnd = 0x0000ffff, kComment = 0xfffe;

// Opcodes (D3DSHADER_INSTRUCTION_OPCODE_TYPE).
enum : DWORD {
  kOpMov = 1,
  kOpDcl = 31,
  kOpSincos = 37,
  kOpMova = 46,
  kOpExpp = 78,
  kOpLogp = 79,
  kOpDef = 81,
  kOpTexldl = 95,
};

// Register types (D3DSHADER_PARAM_REGISTER_TYPE).
enum : DWORD {
  kRegTemp = 0,
  kRegAddress = 3,
  kRegRastOut = 4,  // vs_1/2: oPos, oFog, oPts
  kRegAttrOut = 5,  // vs_1/2: oD0, oD1
  kRegOutput = 6,   // vs_1/2: oT#; vs_3_0: o#
};
constexpr DWORD kRelative = 1u << 13; // D3DSHADER_ADDRMODE_RELATIVE

DWORD registerType(DWORD token) { return ((token >> 28) & 7) | ((token >> 8) & 0x18); }
DWORD registerNumber(DWORD token) { return token & 0x7ff; }
DWORD withRegister(DWORD token, DWORD type, DWORD number) {
  return (token & ~(0x70000000u | 0x1800u | 0x7ffu)) | ((type & 7) << 28) | (((type >> 3) & 3) << 11) | number;
}

// vs_1_x has no instruction lengths: the parameters of its instructions.
int model1Parameters(DWORD op) {
  switch (op) {
  case 0: return 0; // nop
  case 1:           // mov
  case 6:           // rcp
  case 7:           // rsq
  case 14:          // exp
  case 15:          // log
  case 16:          // lit
  case 19:          // frc
  case kOpExpp:
  case kOpLogp:
  case kOpDcl: return 2;
  case 2:  // add
  case 3:  // sub
  case 5:  // mul
  case 8:  // dp3
  case 9:  // dp4
  case 10: // min
  case 11: // max
  case 12: // slt
  case 13: // sge
  case 17: // dst
  case 20: // m4x4
  case 21: // m4x3
  case 22: // m3x4
  case 23: // m3x3
  case 24: return 3; // m3x2
  case 4:            // mad
  case 18: return 4; // lrp
  case kOpDef: return 5;
  default: return -1;
  }
}

// The vs_3_0 output registers translated shaders write.
constexpr DWORD kOutPosition = 8, kOutColor0 = 9, kOutColor1 = 10, kOutFog = 11;
// oPts has no vs_3_0 output left: its writes go to a free temporary.
constexpr DWORD kPointSizeTemp = 31;

// The private device's viewport: ProcessVertices takes positions through it.
constexpr float kViewport = 64;

void multiplyRow(const float in[4], const D3DMATRIX &m, float out[4]) {
  for (int c = 0; c < 4; ++c)
    out[c] = in[0] * m.m[0][c] + in[1] * m.m[1][c] + in[2] * m.m[2][c] + in[3] * m.m[3][c];
}

// The output vertex layout for an output mask: the position (POSITIONT,
// float4), then the colour (D3DCOLOR), texcoord (float2) and normal (float3)
// it has.
struct OutputLayout {
  uint32_t color = 0, texcoord = 0, normal = 0, stride = 16;
};
OutputLayout outputLayout(uint32_t outputs) {
  OutputLayout l;
  if (outputs & kOutputColor)
    l.color = l.stride, l.stride += 4;
  if (outputs & kOutputTexcoord)
    l.texcoord = l.stride, l.stride += 8;
  if (outputs & kOutputNormal)
    l.normal = l.stride, l.stride += 12;
  return l;
}

} // namespace

std::vector<DWORD> shaderTokens(const DWORD *code, size_t limit) {
  if (!code || (code[0] >> 16) != 0xfffe)
    return {};
  const uint32_t major = (code[0] >> 8) & 0xff;
  size_t at = 1;
  while (at < limit) {
    const DWORD token = code[at];
    if (token == kEnd)
      return std::vector<DWORD>(code, code + at + 1);
    if ((token & 0xffff) == kComment) {
      at += 1 + ((token >> 16) & 0x7fff);
      continue;
    }
    if (major >= 2) {
      at += 1 + ((token >> 24) & 0xf);
    } else {
      const int parameters = model1Parameters(token & 0xffff);
      if (parameters < 0)
        return {};
      at += 1 + parameters;
    }
  }
  return {};
}

std::vector<DWORD> toShaderModel3(const std::vector<DWORD> &in, uint32_t &outputs) {
  outputs = 0;
  if (in.empty())
    return {};
  const uint32_t major = (in[0] >> 8) & 0xff;
  if (major >= 3)
    return {};

  // The instructions, each as its opcode token and parameters.
  struct Instruction {
    DWORD token;
    std::vector<DWORD> parameters;
  };
  std::vector<Instruction> body;
  bool written[12] = {}; // the vs_3_0 outputs written
  for (size_t at = 1; at < in.size();) {
    const DWORD token = in[at];
    if (token == kEnd)
      break;
    if ((token & 0xffff) == kComment) {
      at += 1 + ((token >> 16) & 0x7fff);
      continue;
    }
    const DWORD op = token & 0xffff;
    const int count = major >= 2 ? int((token >> 24) & 0xf) : model1Parameters(op);
    if (count < 0 || at + 1 + count > in.size())
      return {};
    Instruction instruction{token, {in.begin() + at + 1, in.begin() + at + 1 + count}};
    at += 1 + count;
    std::vector<DWORD> &p = instruction.parameters;

    if (op == kOpDcl || op == kOpDef || op == 47 || op == 48) {
      // Declarations and constants keep their parameters.
    } else if (op == kOpSincos && p.size() == 4) {
      // vs_2_x sincos takes two helper constants vs_3_0 does not.
      p.resize(2);
    } else if (!p.empty()) {
      // The destination: outputs as vs_3_0 o#, vs_1_x's a0 moves as mova.
      DWORD &destination = p[0];
      const DWORD type = registerType(destination), number = registerNumber(destination);
      if (type == kRegRastOut) {
        const DWORD target = number == 0 ? kOutPosition : number == 1 ? kOutFog : kPointSizeTemp;
        destination = withRegister(destination, number == 2 ? kRegTemp : kRegOutput, target);
        if (number < 2)
          written[target] = true;
      } else if (type == kRegAttrOut) {
        destination = withRegister(destination, kRegOutput, kOutColor0 + number);
        written[kOutColor0 + number] = true;
      } else if (type == kRegOutput) {
        if (number < 8)
          written[number] = true;
      } else if (type == kRegAddress && op == kOpMov) {
        instruction.token = (instruction.token & ~0xffffu) | kOpMova;
      }
      // vs_1_x relative addressing reads a0.x without naming it; later
      // models name it in the token after the parameter.
      if (major < 2) {
        std::vector<DWORD> expanded;
        for (size_t i = 0; i < p.size(); ++i) {
          expanded.push_back(p[i]);
          if (i > 0 && (p[i] & kRelative))
            expanded.push_back(0x80000000u | (kRegAddress << 28)); // a0.x
        }
        p = std::move(expanded);
      }
    }
    body.push_back(std::move(instruction));
  }

  std::vector<DWORD> out = {0xfffe0300};
  // The outputs' declarations: dcl_<usage><index> o#.
  auto declare = [&](DWORD usage, DWORD index, DWORD reg, DWORD mask) {
    out.push_back(kOpDcl | (2u << 24));
    out.push_back(0x80000000u | usage | (index << 16));
    out.push_back(withRegister(0x80000000u | (mask << 16), kRegOutput, reg));
  };
  for (DWORD i = 0; i < 8; ++i)
    if (written[i])
      declare(D3DDECLUSAGE_TEXCOORD, i, i, 0xf);
  if (written[kOutPosition])
    declare(D3DDECLUSAGE_POSITION, 0, kOutPosition, 0xf);
  if (written[kOutColor0])
    declare(D3DDECLUSAGE_COLOR, 0, kOutColor0, 0xf);
  if (written[kOutColor1])
    declare(D3DDECLUSAGE_COLOR, 1, kOutColor1, 0xf);
  if (written[kOutFog])
    declare(D3DDECLUSAGE_FOG, 0, kOutFog, 0x1);
  if (!written[kOutPosition])
    return {};
  for (const Instruction &instruction : body) {
    const DWORD length = DWORD(instruction.parameters.size());
    if (length > 15)
      return {};
    out.push_back((instruction.token & ~0x0f000000u) | (length << 24));
    out.insert(out.end(), instruction.parameters.begin(), instruction.parameters.end());
  }
  out.push_back(kEnd);
  outputs = (written[kOutColor0] ? kOutputColor : 0) | (written[0] ? kOutputTexcoord : 0);
  return out;
}

ShaderCode parseShader(const DWORD *code) {
  ShaderCode s;
  s.tokens = shaderTokens(code);
  if (s.tokens.empty())
    return s;
  s.hash = XXH3_64bits(s.tokens.data(), s.tokens.size() * sizeof(DWORD));
  s.major = (s.tokens[0] >> 8) & 0xff;
  s.minor = s.tokens[0] & 0xff;
  if (s.major < 3) {
    s.model3 = toShaderModel3(s.tokens, s.outputs);
    return s;
  }
  // vs_3_0: its own declarations name its outputs.
  bool position = false;
  for (size_t at = 1; at < s.tokens.size();) {
    const DWORD token = s.tokens[at];
    if (token == kEnd)
      break;
    if ((token & 0xffff) == kComment) {
      at += 1 + ((token >> 16) & 0x7fff);
      continue;
    }
    const DWORD op = token & 0xffff, length = (token >> 24) & 0xf;
    if (op == kOpTexldl)
      s.vertexTextures = true;
    if (op == kOpDcl && length >= 2 && at + 2 < s.tokens.size()) {
      const DWORD usage = s.tokens[at + 1], destination = s.tokens[at + 2];
      if (registerType(destination) == kRegOutput && ((usage >> 16) & 0xf) == 0) {
        switch (usage & 0x1f) {
        case D3DDECLUSAGE_POSITION: position = true; break;
        case D3DDECLUSAGE_COLOR: s.outputs |= kOutputColor; break;
        case D3DDECLUSAGE_TEXCOORD: s.outputs |= kOutputTexcoord; break;
        case D3DDECLUSAGE_NORMAL: s.outputs |= kOutputNormal; break;
        default: break;
        }
      }
    }
    at += 1 + length;
  }
  if (position && !s.vertexTextures)
    s.model3 = s.tokens;
  return s;
}

bool invert(const D3DMATRIX &in, D3DMATRIX &out) {
  double a[4][8];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      a[r][c] = in.m[r][c];
      a[r][c + 4] = r == c ? 1 : 0;
    }
  for (int c = 0; c < 4; ++c) {
    int pivot = c;
    for (int r = c + 1; r < 4; ++r)
      if (std::fabs(a[r][c]) > std::fabs(a[pivot][c]))
        pivot = r;
    if (std::fabs(a[pivot][c]) < 1e-20)
      return false;
    if (pivot != c)
      for (int k = 0; k < 8; ++k)
        std::swap(a[c][k], a[pivot][k]);
    const double scale = 1 / a[c][c];
    for (int k = 0; k < 8; ++k)
      a[c][k] *= scale;
    for (int r = 0; r < 4; ++r)
      if (r != c) {
        const double f = a[r][c];
        for (int k = 0; k < 8; ++k)
          a[r][k] -= f * a[c][k];
      }
  }
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c)
      out.m[r][c] = float(a[r][c + 4]);
  return true;
}

Unprojection makeUnprojection(const D3DMATRIX &projection, const D3DMATRIX &view, const D3DMATRIX &world,
                              bool worldMatrices) {
  Unprojection u;
  if (!invert(projection, u.inverseProjection) || !invert(view, u.viewToWorld))
    return u;
  if (!worldMatrices || !invert(world, u.worldToObject))
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c)
        u.worldToObject.m[r][c] = r == c ? 1.0f : 0.0f;
  u.valid = true;
  return u;
}

void unproject(const Unprojection &u, const float clip[4], float out[3]) {
  float viewH[4], worldH[4], objectH[4];
  multiplyRow(clip, u.inverseProjection, viewH);
  const float w = viewH[3] != 0 ? viewH[3] : 1e-20f;
  const float viewPoint[4] = {viewH[0] / w, viewH[1] / w, viewH[2] / w, 1};
  multiplyRow(viewPoint, u.viewToWorld, worldH);
  const float worldPoint[4] = {worldH[0], worldH[1], worldH[2], 1};
  multiplyRow(worldPoint, u.worldToObject, objectH);
  out[0] = objectH[0], out[1] = objectH[1], out[2] = objectH[2];
}

VertexCapture::~VertexCapture() { release(); }

void VertexCapture::release() {
  auto drop = [](auto *&object) {
    if (object)
      object->Release();
    object = nullptr;
  };
  for (auto &stream : m_streams)
    drop(stream);
  drop(m_output);
  for (auto &declaration : m_outputs)
    drop(declaration);
  for (auto &[hash, shader] : m_shaders)
    drop(shader);
  for (auto &[hash, declaration] : m_declarations)
    drop(declaration);
  m_shaders.clear();
  m_declarations.clear();
  std::fill(std::begin(m_streamSizes), std::end(m_streamSizes), 0u);
  m_outputVertices = 0;
  drop(m_device);
}

bool VertexCapture::ensureDevice(IDirect3DDevice9 *game) {
  if (m_device || m_failed)
    return m_device != nullptr;
  m_failed = true;
  IDirect3D9 *d3d = nullptr;
  if (!game || FAILED(game->GetDirect3D(&d3d)) || !d3d)
    return false;
  D3DDEVICE_CREATION_PARAMETERS creation = {};
  game->GetCreationParameters(&creation);
  // A small window of its own: the device never draws or presents.
  static HWND window = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr,
                                       GetModuleHandleW(nullptr), nullptr);
  D3DPRESENT_PARAMETERS pp = {};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.BackBufferWidth = pp.BackBufferHeight = UINT(kViewport);
  pp.BackBufferFormat = D3DFMT_UNKNOWN;
  pp.hDeviceWindow = window;
  const HRESULT hr = d3d->CreateDevice(creation.AdapterOrdinal, D3DDEVTYPE_HAL, window,
                                       D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &m_device);
  d3d->Release();
  if (FAILED(hr))
    return false;
  // Positions leave through a known viewport; nothing is clipped or lit.
  const D3DVIEWPORT9 viewport = {0, 0, DWORD(kViewport), DWORD(kViewport), 0, 1};
  m_device->SetViewport(&viewport);
  m_device->SetRenderState(D3DRS_CLIPPING, FALSE);
  m_device->SetRenderState(D3DRS_LIGHTING, FALSE);
  m_failed = false;
  return true;
}

IDirect3DVertexBuffer9 *VertexCapture::streamBuffer(uint32_t stream, uint32_t bytes) {
  if (m_streamSizes[stream] < bytes) {
    if (m_streams[stream])
      m_streams[stream]->Release();
    m_streams[stream] = nullptr;
    const uint32_t size = std::max(bytes, m_streamSizes[stream] * 2);
    if (FAILED(m_device->CreateVertexBuffer(size, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY | D3DUSAGE_SOFTWAREPROCESSING,
                                            0, D3DPOOL_SYSTEMMEM, &m_streams[stream], nullptr))) {
      m_streamSizes[stream] = 0;
      return nullptr;
    }
    m_streamSizes[stream] = size;
  }
  return m_streams[stream];
}

bool VertexCapture::run(const Draw &d, uint32_t first, uint32_t count, std::vector<CapturedVertex> &out) {
  if (!d.shader || d.shader->model3.empty() || !count || !ensureDevice(d.device))
    return false;
  // The shader (as vs_3_0), the declaration and the output layout on this device.
  auto shader = m_shaders.find(d.shader->hash);
  if (shader == m_shaders.end()) {
    IDirect3DVertexShader9 *created = nullptr;
    m_device->CreateVertexShader(d.shader->model3.data(), &created);
    shader = m_shaders.emplace(d.shader->hash, created).first;
  }
  auto declaration = m_declarations.find(d.declarationHash);
  if (declaration == m_declarations.end()) {
    IDirect3DVertexDeclaration9 *created = nullptr;
    m_device->CreateVertexDeclaration(d.elements, &created);
    declaration = m_declarations.emplace(d.declarationHash, created).first;
  }
  const uint32_t outputs = d.shader->outputs & 7;
  const OutputLayout layout = outputLayout(outputs);
  if (!m_outputs[outputs]) {
    D3DVERTEXELEMENT9 elements[5] = {{0, 0, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_POSITIONT, 0}};
    int n = 1;
    if (outputs & kOutputColor)
      elements[n++] = {0, WORD(layout.color), D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0};
    if (outputs & kOutputTexcoord)
      elements[n++] = {0, WORD(layout.texcoord), D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 0};
    if (outputs & kOutputNormal)
      elements[n++] = {0, WORD(layout.normal), D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_NORMAL, 0};
    elements[n] = D3DDECL_END();
    m_device->CreateVertexDeclaration(elements, &m_outputs[outputs]);
  }
  if (!shader->second || !declaration->second || !m_outputs[outputs])
    return false;

  // The vertices [first, first + count) of each stream the declaration reads,
  // copied to the start of this device's buffers.
  uint32_t used = 0;
  for (const D3DVERTEXELEMENT9 *e = d.elements; e->Stream != 0xff; ++e)
    if (e->Stream < kStreams)
      used |= 1u << e->Stream;
  for (uint32_t i = 0; i < kStreams; ++i) {
    if (!(used & (1u << i))) {
      m_device->SetStreamSource(i, nullptr, 0, 0);
      continue;
    }
    const VertexStream &s = d.streams[i];
    const uint64_t begin = uint64_t(s.offset) + uint64_t(s.stride) * first;
    const uint64_t bytes = uint64_t(s.stride) * count;
    if (!s.data || !s.stride || begin >= s.size)
      return false;
    const uint32_t available = uint32_t(std::min<uint64_t>(bytes, s.size - begin));
    IDirect3DVertexBuffer9 *buffer = streamBuffer(i, uint32_t(bytes));
    void *data = nullptr;
    if (!buffer || FAILED(buffer->Lock(0, uint32_t(bytes), &data, D3DLOCK_DISCARD)))
      return false;
    std::memcpy(data, s.data + begin, available);
    if (available < bytes)
      std::memset(static_cast<uint8_t *>(data) + available, 0, size_t(bytes - available));
    buffer->Unlock();
    m_device->SetStreamSource(i, buffer, 0, s.stride);
  }
  // The largest output layout (36 bytes) per vertex.
  if (m_outputVertices < count) {
    if (m_output)
      m_output->Release();
    m_output = nullptr;
    m_outputVertices = std::max(count, m_outputVertices * 2);
    if (FAILED(m_device->CreateVertexBuffer(m_outputVertices * 40, D3DUSAGE_SOFTWAREPROCESSING, 0, D3DPOOL_SYSTEMMEM,
                                            &m_output, nullptr))) {
      m_outputVertices = 0;
      return false;
    }
  }
  m_device->SetVertexDeclaration(declaration->second);
  m_device->SetVertexShader(shader->second);
  m_device->SetVertexShaderConstantF(0, d.constantsF, 256);
  m_device->SetVertexShaderConstantI(0, d.constantsI, 16);
  m_device->SetVertexShaderConstantB(0, d.constantsB, 16);
  if (FAILED(m_device->ProcessVertices(0, 0, count, m_output, m_outputs[outputs], D3DPV_DONOTCOPYDATA)))
    return false;

  void *data = nullptr;
  if (FAILED(m_output->Lock(0, count * layout.stride, &data, D3DLOCK_READONLY)))
    return false;
  out.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t *v = static_cast<const uint8_t *>(data) + size_t(i) * layout.stride;
    CapturedVertex &c = out[i];
    c = {};
    float position[4];
    std::memcpy(position, v, 16);
    // Back through the viewport and the perspective divide.
    const float w = position[3] != 0 ? 1 / position[3] : 1;
    const float ndc[3] = {position[0] / kViewport * 2 - 1, 1 - position[1] / kViewport * 2, position[2]};
    c.clip[0] = ndc[0] * w, c.clip[1] = ndc[1] * w, c.clip[2] = ndc[2] * w, c.clip[3] = w;
    c.color = 0xffffffffu;
    if (outputs & kOutputColor) {
      D3DCOLOR color;
      std::memcpy(&color, v + layout.color, 4);
      // D3DCOLOR (B, G, R, A in memory) as RGBA.
      c.color = (color & 0xff00ff00u) | (color >> 16 & 0xff) | (color & 0xff) << 16;
    }
    if (outputs & kOutputTexcoord)
      std::memcpy(c.texcoord, v + layout.texcoord, 8);
    if (outputs & kOutputNormal)
      std::memcpy(c.normal, v + layout.normal, 12);
  }
  m_output->Unlock();
  return true;
}

} // namespace openremix::d3d9
