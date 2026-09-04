signalsmith-linear — https://github.com/Signalsmith-Audio/linear —
commit ec38d3332c7e9ce88a2fd969003d170baa2316b6 (main, fetched 2026-09-04) — MIT
(see LICENSE.txt). Header-only, unmodified.

Sources (all fetched verbatim, recursively, until self-contained):
- https://raw.githubusercontent.com/Signalsmith-Audio/linear/main/stft.h
- https://raw.githubusercontent.com/Signalsmith-Audio/linear/main/linear.h
- https://raw.githubusercontent.com/Signalsmith-Audio/linear/main/fft.h
- https://raw.githubusercontent.com/Signalsmith-Audio/linear/main/approx.h
- https://raw.githubusercontent.com/Signalsmith-Audio/linear/main/include/signalsmith-linear/stft.h
- https://raw.githubusercontent.com/Signalsmith-Audio/linear/main/include/signalsmith-linear/linear.h
- https://raw.githubusercontent.com/Signalsmith-Audio/linear/main/include/signalsmith-linear/fft.h
- https://raw.githubusercontent.com/Signalsmith-Audio/linear/main/include/signalsmith-linear/approx.h

Pulled in by `signalsmith-stretch/signalsmith-stretch.h` via
`#include "signalsmith-linear/stft.h"`. Upstream ships two copies of each
header: a real implementation at the repo root (`stft.h`, `linear.h`, `fft.h`,
`approx.h`) and a thin forwarding header at
`include/signalsmith-linear/<name>.h` (`#include "../../<name>.h"`) that
exists purely to give consumers the `signalsmith-linear/<name>.h` include
path signalsmith-stretch expects. Both layers are vendored, in the same
relative layout as upstream, so that forwarding path resolves without
editing either file.

Not linked into anything on its own; only reached indirectly, through
`signalsmith-stretch`, into `engine/`.

Approved by owner 2026-09-04 (docs/decisions/0005).
