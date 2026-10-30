#pragma once
#include "config.h"
#include "draw.h"

// Which camera a draw is drawn with, and whether it is sky, by Remix Plus's
// rules (src/frontends/translator/camera_classifier.cpp, sky_rules.cpp and
// dxvk-remix shouldBakeSky).
namespace openremix::d3d9 {

enum class CameraClass { Main, ViewModel, Sky, Unknown };

// The frame's cameras as the classifier saw them.
struct CameraFrame {
  bool mainSet = false;   // a Main camera was set this frame
  bool mainKnown = false; // a Main camera was ever set
  float mainFov = 0;      // its vertical field of view (radians)
  void endFrame() { mainSet = false; }
};

// The vertical field of view and horizontal shear of a D3D projection.
void decomposeProjection(const D3DMATRIX &projection, float &fov, float &shearX);

// The draw's sky rules: a viewport from rtx.skyMinZThreshold on, a sky box
// texture (`categories` has SKY), or an untextured draw before
// rtx.skyDrawcallIdThreshold.
bool skyDraw(const Draw &draw, const Config &config, uint32_t categories);

// The draw's camera. `updatesCamera`: its matrices describe a camera (false
// for Unknown, and for a skinned draw once a Main camera is known).
CameraClass classifyCamera(const Draw &draw, const Config &config, bool sky, bool skinned, CameraFrame &frame,
                           bool &updatesCamera);

} // namespace openremix::d3d9
