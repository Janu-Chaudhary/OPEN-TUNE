signalsmith-stretch — https://github.com/Signalsmith-Audio/signalsmith-stretch —
commit 57b93f4e9206a089a45387eaa39bdc9f310d3308 (main, fetched 2026-09-04) — MIT
(see LICENSE.txt). Header-only, unmodified.

Source: https://raw.githubusercontent.com/Signalsmith-Audio/signalsmith-stretch/main/signalsmith-stretch.h

Linked into `engine/` (constitution IV permits vendored DSP libraries there —
unlike `dr_wav`, this is not host-only). Depends on `signalsmith-linear`
(vendored separately at `third_party/signalsmith-linear/`) via
`#include "signalsmith-linear/stft.h"`; the include path is preserved
unmodified so that include line resolves without editing this file.

`cmd/util` (the upstream repo's CLI-demo helper) is intentionally NOT
vendored — it is not used by `SignalsmithCorrector` and pulling it in would
add an unused dependency.

Approved by owner 2026-09-04 (docs/decisions/0005).
