# Free camera (experimental)

Full write-up: `runtime/NOTES.md` "Câmera livre em terceira pessoa"; GUIA "Câmera livre (experimental)".
Code: `runtime/camera/free_camera.{h,cpp}`; setting `[camera] free_camera` (off by default), CÂMERA tab.
Profiles `camera_livre`, `camera_livre_abertura`.

- Managers table 0x824A7A24, manager i at +28 + 4i: 4 camera, 5 collision, 12 render, 18 cinematics.
- `CRenderManager+356` = `pure3d::VectorCamera`: +216 position, +228 direction, +240 up, +208 matrices-valid.
- `CCameraBehaviourController::vf08` sub_820E0498, phase 3, writes it (not called during cinematics).
- Walking (sub_821EBC70) uses that camera, so overriding it makes movement camera-relative for free.
- World ray test: sub_821C5EA0(coll + 4, {from, to, -1, -1, 0}, hit, 1) -> hit[0] = distance.
- R3 (recenter) must be read through `input::MenuButtons`: input-map channels move with the control scheme.
- Mounted Crash's actor rides on the titan (position follows the titan).
- Extend `free_camera.cpp` instead of adding a second `PPC_FUNC` for these functions.
