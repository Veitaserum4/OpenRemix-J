#include "camera.h"

#include <remix/remix_c.h>

#include <cmath>
#include <cstring>

namespace openremix::d3d9 {
namespace {

// Two fields of view closer than this are the same camera's.
constexpr float kFovTolerance = 0.001f;

bool identity(const D3DMATRIX &m) {
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c)
      if (m.m[r][c] != (r == c ? 1.0f : 0.0f))
        return false;
  return true;
}

D3DMATRIX multiply(const D3DMATRIX &a, const D3DMATRIX &b) {
  D3DMATRIX out;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      float s = 0;
      for (int k = 0; k < 4; ++k)
        s += a.m[r][k] * b.m[k][c];
      out.m[r][c] = s;
    }
  return out;
}

} // namespace

void decomposeProjection(const D3DMATRIX &p, float &fov, float &shearX) {
  // A D3D projection (row vectors): _22 = cot(fov / 2), _31 the off-centre
  // shift in x relative to _11's scale.
  fov = p._22 != 0 ? 2 * std::atan(1 / std::fabs(p._22)) : 0;
  shearX = p._11 != 0 ? p._31 / p._11 : 0;
}

bool skyDraw(const Draw &d, const Config &c, uint32_t categories) {
  if (d.viewport.MinZ >= c.skyMinZThreshold)
    return true;
  if (categories & REMIXAPI_INSTANCE_CATEGORY_BIT_SKY)
    return true;
  return !d.texture0 && d.drawId < c.skyDrawcallIdThreshold;
}

CameraClass classifyCamera(const Draw &d, const Config &c, bool sky, bool skinned, CameraFrame &frame,
                           bool &updatesCamera) {
  updatesCamera = false;
  const CameraClass rejected = sky ? CameraClass::Sky : CameraClass::Unknown;
  if (identity(d.projection))
    return rejected;
  // Without fused matrices, a world-view equal to the world (and not
  // identity) is no camera.
  if (c.fusedWorldViewMode == 0) {
    const D3DMATRIX objectToView = multiply(d.world, d.view);
    if (std::memcmp(&objectToView, &d.world, sizeof(D3DMATRIX)) == 0 && !identity(objectToView))
      return rejected;
  }
  float fov, shear;
  decomposeProjection(d.projection, fov, shear);
  // Extreme shearing and degenerate fields of view are no camera.
  if (std::fabs(shear) > 0.01f || !(fov >= kFovTolerance))
    return rejected;

  updatesCamera = !skinned || !frame.mainKnown;
  CameraClass camera = CameraClass::Main;
  if (sky) {
    camera = CameraClass::Sky;
  } else if (c.viewModelEnable &&
             (d.viewport.MaxZ <= c.viewModelMaxZThreshold ||
              (frame.mainSet && !(std::fabs(fov - frame.mainFov) < kFovTolerance)))) {
    camera = CameraClass::ViewModel;
  }
  if (updatesCamera && camera == CameraClass::Main && !frame.mainSet) {
    frame.mainSet = frame.mainKnown = true;
    frame.mainFov = fov;
  }
  return camera;
}

} // namespace openremix::d3d9
