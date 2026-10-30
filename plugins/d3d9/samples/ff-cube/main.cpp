// A fixed-function D3D9 app to run the plugin against: a textured cube lit by
// a point light, standing on a lit floor, a pyramid drawn without indices, a
// column bent by vertex blending (two world matrices), a box held in front of
// the camera with its own field of view (a view model), and a
// pre-transformed HUD bar. Runs for `--frames N` frames (default: until
// closed).
#include <windows.h>

#include <d3d9.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>

namespace {

constexpr UINT kWidth = 1280, kHeight = 720;

struct Vertex {
  float x, y, z, nx, ny, nz, u, v;
};
constexpr DWORD kVertexFvf = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1;

// A vertex blended between D3DTS_WORLDMATRIX(0) (weight) and (1).
struct BlendVertex {
  float x, y, z, weight, nx, ny, nz, u, v;
};
constexpr DWORD kBlendFvf = D3DFVF_XYZB1 | D3DFVF_NORMAL | D3DFVF_TEX1;

struct HudVertex {
  float x, y, z, rhw;
  D3DCOLOR color;
};
constexpr DWORD kHudFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM w, LPARAM l) {
  if (message == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(window, message, w, l);
}

D3DMATRIX identity() {
  D3DMATRIX m = {};
  m._11 = m._22 = m._33 = m._44 = 1;
  return m;
}

// Left-handed look-at and perspective (D3DXMatrixLookAtLH / PerspectiveFovLH).
D3DMATRIX lookAt(const float eye[3], const float at[3]) {
  float z[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
  float length = std::sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
  for (float &c : z)
    c /= length;
  float x[3] = {z[2], 0, -z[0]}; // up (0, 1, 0) x z
  length = std::sqrt(x[0] * x[0] + x[2] * x[2]);
  x[0] /= length, x[2] /= length;
  const float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
  D3DMATRIX m = identity();
  m._11 = x[0], m._21 = x[1], m._31 = x[2];
  m._12 = y[0], m._22 = y[1], m._32 = y[2];
  m._13 = z[0], m._23 = z[1], m._33 = z[2];
  m._41 = -(x[0] * eye[0] + x[1] * eye[1] + x[2] * eye[2]);
  m._42 = -(y[0] * eye[0] + y[1] * eye[1] + y[2] * eye[2]);
  m._43 = -(z[0] * eye[0] + z[1] * eye[1] + z[2] * eye[2]);
  return m;
}

D3DMATRIX perspective(float fovY, float aspect, float zn, float zf) {
  const float y = 1 / std::tan(fovY / 2);
  D3DMATRIX m = {};
  m._11 = y / aspect;
  m._22 = y;
  m._33 = zf / (zf - zn);
  m._34 = 1;
  m._43 = -zn * zf / (zf - zn);
  return m;
}

D3DMATRIX translation(float x, float y, float z) {
  D3DMATRIX m = identity();
  m._41 = x, m._42 = y, m._43 = z;
  return m;
}

D3DMATRIX multiply(const D3DMATRIX &a, const D3DMATRIX &b) {
  D3DMATRIX out = {};
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c)
      for (int k = 0; k < 4; ++k)
        out.m[r][c] += a.m[r][k] * b.m[k][c];
  return out;
}

// The inverse of a rigid view matrix (rotation and translation).
D3DMATRIX inverseView(const D3DMATRIX &v) {
  D3DMATRIX m = identity();
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c)
      m.m[r][c] = v.m[c][r];
  for (int c = 0; c < 3; ++c)
    m.m[3][c] = -(v.m[3][0] * m.m[0][c] + v.m[3][1] * m.m[1][c] + v.m[3][2] * m.m[2][c]);
  return m;
}

D3DMATRIX rotationZ(float angle) {
  D3DMATRIX m = identity();
  m._11 = std::cos(angle), m._12 = std::sin(angle);
  m._21 = -std::sin(angle), m._22 = std::cos(angle);
  return m;
}

D3DMATRIX rotationY(float angle) {
  D3DMATRIX m = identity();
  m._11 = std::cos(angle), m._13 = -std::sin(angle);
  m._31 = std::sin(angle), m._33 = std::cos(angle);
  return m;
}

// A cube of side 2 around the origin, lifted by `lift`; 24 vertices, 36 indices.
void cube(Vertex *v, uint16_t *indices, float lift) {
  static const float faces[6][3][3] = {
      // normal, u axis, v axis
      {{0, 0, -1}, {1, 0, 0}, {0, -1, 0}}, {{0, 0, 1}, {-1, 0, 0}, {0, -1, 0}}, {{-1, 0, 0}, {0, 0, -1}, {0, -1, 0}},
      {{1, 0, 0}, {0, 0, 1}, {0, -1, 0}},  {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},  {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
  };
  for (int f = 0; f < 6; ++f) {
    const float *n = faces[f][0], *a = faces[f][1], *b = faces[f][2];
    for (int k = 0; k < 4; ++k) {
      const float s = (k & 1) ? 1.0f : -1.0f, t = (k & 2) ? 1.0f : -1.0f;
      Vertex &out = v[f * 4 + k];
      out = {n[0] + s * a[0] + t * b[0], n[1] + s * a[1] + t * b[1] + lift, n[2] + s * a[2] + t * b[2],
             n[0], n[1], n[2], (s + 1) / 2, (t + 1) / 2};
    }
    const uint16_t base = uint16_t(f * 4), quad[6] = {0, 1, 2, 2, 1, 3};
    for (int k = 0; k < 6; ++k)
      indices[f * 6 + k] = uint16_t(base + quad[k]);
  }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  int frames = -1;
  for (int i = 1; i + 1 < __argc; ++i)
    if (std::wcscmp(__wargv[i], L"--frames") == 0)
      frames = _wtoi(__wargv[i + 1]);

  WNDCLASSW wc = {};
  wc.lpfnWndProc = windowProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.lpszClassName = L"FfCube";
  RegisterClassW(&wc);
  RECT rect = {0, 0, LONG(kWidth), LONG(kHeight)};
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  HWND window = CreateWindowW(wc.lpszClassName, L"ff-cube (D3D9 fixed function)", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                              CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance,
                              nullptr);
  ShowWindow(window, SW_SHOW);

  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d)
    return 1;
  D3DPRESENT_PARAMETERS pp = {};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferWidth = kWidth;
  pp.BackBufferHeight = kHeight;
  pp.EnableAutoDepthStencil = TRUE;
  pp.AutoDepthStencilFormat = D3DFMT_D24S8;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
  IDirect3DDevice9 *device = nullptr;
  if (FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp,
                               &device)))
    return 2;

  // Geometry: the cube and a floor quad, in managed buffers.
  Vertex vertices[28];
  uint16_t indices[42];
  cube(vertices, indices, 1);
  const float floor[4][2] = {{-8, -8}, {8, -8}, {-8, 8}, {8, 8}};
  for (int k = 0; k < 4; ++k)
    vertices[24 + k] = {floor[k][0], 0, floor[k][1], 0, 1, 0, floor[k][0] / 2, floor[k][1] / 2};
  const uint16_t floorIndices[6] = {24, 26, 25, 25, 26, 27};
  std::memcpy(indices + 36, floorIndices, sizeof(floorIndices));
  IDirect3DVertexBuffer9 *vb = nullptr;
  IDirect3DIndexBuffer9 *ib = nullptr;
  device->CreateVertexBuffer(sizeof(vertices), D3DUSAGE_WRITEONLY, kVertexFvf, D3DPOOL_MANAGED, &vb, nullptr);
  device->CreateIndexBuffer(sizeof(indices), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, nullptr);
  void *data = nullptr;
  vb->Lock(0, 0, &data, 0);
  std::memcpy(data, vertices, sizeof(vertices));
  vb->Unlock();
  ib->Lock(0, 0, &data, 0);
  std::memcpy(data, indices, sizeof(indices));
  ib->Unlock();

  // A checker texture, and a plain one for the floor.
  IDirect3DTexture9 *checker = nullptr, *plain = nullptr;
  device->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &checker, nullptr);
  device->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &plain, nullptr);
  D3DLOCKED_RECT locked;
  checker->LockRect(0, &locked, nullptr, 0);
  for (int y = 0; y < 64; ++y)
    for (int x = 0; x < 64; ++x)
      reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(locked.pBits) + y * locked.Pitch)[x] =
          ((x / 8 + y / 8) & 1) ? 0xffd04020u : 0xfff0f0f0u;
  checker->UnlockRect(0);
  plain->LockRect(0, &locked, nullptr, 0);
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 4; ++x)
      reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(locked.pBits) + y * locked.Pitch)[x] = 0xff808080u;
  plain->UnlockRect(0);

  // A pyramid (no index buffer): four sides around (3, 0, 0).
  Vertex pyramid[12];
  {
    const float apex[3] = {3, 1.5f, 0}, base[4][3] = {{2.2f, 0, -0.8f}, {3.8f, 0, -0.8f}, {3.8f, 0, 0.8f}, {2.2f, 0, 0.8f}};
    for (int f = 0; f < 4; ++f) {
      const float *a = base[f], *b = base[(f + 1) % 4];
      const float ux = a[0] - apex[0], uy = a[1] - apex[1], uz = a[2] - apex[2];
      const float vx = b[0] - apex[0], vy = b[1] - apex[1], vz = b[2] - apex[2];
      float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
      const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
      nx /= length, ny /= length, nz /= length;
      pyramid[f * 3 + 0] = {apex[0], apex[1], apex[2], nx, ny, nz, 0.5f, 0};
      pyramid[f * 3 + 1] = {a[0], a[1], a[2], nx, ny, nz, 0, 1};
      pyramid[f * 3 + 2] = {b[0], b[1], b[2], nx, ny, nz, 1, 1};
    }
  }
  IDirect3DVertexBuffer9 *pyramidVb = nullptr;
  device->CreateVertexBuffer(sizeof(pyramid), D3DUSAGE_WRITEONLY, kVertexFvf, D3DPOOL_MANAGED, &pyramidVb, nullptr);
  pyramidVb->Lock(0, 0, &data, 0);
  std::memcpy(data, pyramid, sizeof(pyramid));
  pyramidVb->Unlock();

  // A column (the cube, narrowed and stretched to 3 units), its bottom on
  // matrix 0 and its top on matrix 1: it bends.
  BlendVertex column[24];
  for (int i = 0; i < 24; ++i) {
    const Vertex &c = vertices[i];
    const float y = c.y * 1.5f;
    column[i] = {c.x * 0.3f, y, c.z * 0.3f, 1 - y / 3, c.nx, c.ny, c.nz, c.u, c.v};
  }
  IDirect3DVertexBuffer9 *columnVb = nullptr;
  device->CreateVertexBuffer(sizeof(column), D3DUSAGE_WRITEONLY, kBlendFvf, D3DPOOL_MANAGED, &columnVb, nullptr);
  columnVb->Lock(0, 0, &data, 0);
  std::memcpy(data, column, sizeof(column));
  columnVb->Unlock();

  D3DLIGHT9 light = {};
  light.Type = D3DLIGHT_POINT;
  light.Diffuse = {1.0f, 0.9f, 0.7f, 1};
  light.Position = {4, 6, -4};
  light.Range = 30;
  light.Attenuation2 = 0.02f;
  D3DMATERIAL9 material = {};
  material.Diffuse = material.Ambient = {1, 1, 1, 1};

  const float eye[3] = {6, 5, -8}, at[3] = {0, 1, 0};
  const D3DMATRIX view = lookAt(eye, at), projection = perspective(1.0f, float(kWidth) / kHeight, 0.1f, 100);

  const HudVertex hud[4] = {{20, 20, 0, 1, 0xc000a0ff}, {320, 20, 0, 1, 0xc000a0ff},
                            {20, 50, 0, 1, 0xc0004080}, {320, 50, 0, 1, 0xc0004080}};

  for (int frame = 0; frames < 0 || frame < frames; ++frame) {
    MSG msg;
    bool quit = false;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT)
        quit = true;
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (quit)
      break;

    device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(30, 30, 40), 1, 0);
    device->BeginScene();
    device->SetTransform(D3DTS_VIEW, &view);
    device->SetTransform(D3DTS_PROJECTION, &projection);
    device->SetRenderState(D3DRS_LIGHTING, TRUE);
    device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    device->SetLight(0, &light);
    device->LightEnable(0, TRUE);
    device->SetMaterial(&material);
    device->SetFVF(kVertexFvf);
    device->SetStreamSource(0, vb, 0, sizeof(Vertex));
    device->SetIndices(ib);

    const D3DMATRIX world = identity();
    device->SetTransform(D3DTS_WORLD, &world);
    device->SetTexture(0, plain);
    device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 24, 4, 36, 2);

    const D3DMATRIX spin = rotationY(frame * 0.01f);
    device->SetTransform(D3DTS_WORLD, &spin);
    device->SetTexture(0, checker);
    device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 24, 0, 12);

    // The pyramid, without indices.
    device->SetTransform(D3DTS_WORLD, &world);
    device->SetTexture(0, checker);
    device->SetStreamSource(0, pyramidVb, 0, sizeof(Vertex));
    device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 4);

    // The column, blended between two world matrices.
    const D3DMATRIX base = translation(-3, 0, 1);
    const D3DMATRIX bend = multiply(rotationZ(std::sin(frame * 0.02f) * 0.6f), base);
    device->SetTransform(D3DTS_WORLDMATRIX(0), &base);
    device->SetTransform(D3DTS_WORLDMATRIX(1), &bend);
    device->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_1WEIGHTS);
    device->SetFVF(kBlendFvf);
    device->SetStreamSource(0, columnVb, 0, sizeof(BlendVertex));
    device->SetTexture(0, checker);
    device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 24, 0, 12);
    device->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);

    // A view model: a small box held in front of the camera (placed in view
    // space, drawn with the camera's view) with its own field of view.
    D3DMATRIX shrink = identity();
    shrink._11 = shrink._22 = shrink._33 = 0.15f;
    const D3DMATRIX held = multiply(multiply(multiply(shrink, translation(0, -0.15f, 0)), rotationY(0.6f)),
                                    translation(0.7f, -0.6f, 2.2f));
    const D3DMATRIX viewModelWorld = multiply(held, inverseView(view)),
                    viewModelProjection = perspective(0.7f, float(kWidth) / kHeight, 0.05f, 10);
    device->SetFVF(kVertexFvf);
    device->SetStreamSource(0, vb, 0, sizeof(Vertex));
    device->SetTransform(D3DTS_WORLD, &viewModelWorld);
    device->SetTransform(D3DTS_PROJECTION, &viewModelProjection);
    device->SetTexture(0, plain);
    device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 24, 0, 12);

    // The HUD: pre-transformed, blended, no lighting or depth.
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetTexture(0, nullptr);
    device->SetFVF(kHudFvf);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, hud, sizeof(HudVertex));
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);

    device->EndScene();
    device->Present(nullptr, nullptr, nullptr, nullptr);
  }

  checker->Release();
  plain->Release();
  pyramidVb->Release();
  columnVb->Release();
  vb->Release();
  ib->Release();
  device->Release();
  d3d->Release();
  DestroyWindow(window);
  return 0;
}
