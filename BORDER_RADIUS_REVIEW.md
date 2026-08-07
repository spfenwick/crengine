# Code Review: border-radius support (PR #629)

Review of changes since `91521dbfc71086981a62c58dc236d76da7e8ab4a` for style,
correctness (including alignment with standard graphics approaches), and
comments. Findings are ranked most severe first.

## 1. Blitter out-of-bounds read (correctness)

**File:** `crengine/src/lvdrawbuf.cpp:635`

The rewritten scanline blitter loops over `[xL, xR)` — the clip rect
intersected with the rounded-clip mask — but never intersects that range
with the image's own `[dst_x, dst_x + dst_dx)` extent before indexing source
data with `relx = x - dst_x`.

**Failure scenario:** Any image draw where the active clip rect is wider
than the image (the normal case — e.g. an inline `<img>` word drawn via
`lvtextfm.cpp`'s `buf->Draw(img, xx, yy, word->width, word->o.height)` with
the paragraph clip active, or `LVColorDrawBuf::Draw()` calls in
`lvdocview.cpp` for thumbnails) makes `xL < dst_x`, so `relx` is negative for
the first iterations, and/or `xR > dst_x + dst_dx`, so `relx` runs past the
end of the array. `data[xmap ? xmap[relx] : relx]` is then read
out-of-bounds, corrupting memory or crashing. The old code was safe because
the for-loop itself ranged over `[0, dst_dx)` and only used the absolute `xx`
for a skip-check; this refactor removed that bound.

## 2. Rounded borders skip color inversion in night mode (correctness)

**File:** `crengine/src/lvrend.cpp:10009`

The new rounded-border drawing path (`fillRoundedRectRing` /
`fillRoundedRectDashedRing` / `fillRoundedRectBorderSidesBand` and dashed
variants) uses `topBordercolor`/`rightBordercolor`/`bottomBordercolor`/
`leftBordercolor` directly and never applies `invertNonGrayscaleColor`,
unlike every corresponding branch in the legacy (non-rounded) border-drawing
code just below it (around lines 10399, 10512, 10627, 10731, all of which
check `invert_colors`).

**Failure scenario:** With night mode / color inversion enabled
(`drawbuf.getInvertColors() == true`, a common e-ink reader mode), any
element with both a border and `border-radius` renders its border in the
un-inverted (wrong) color — breaking night-mode rendering specifically for
rounded-corner borders.

## 3. Rounded groove/ridge borders lose black shading override (correctness)

**File:** `crengine/src/lvrend.cpp:10084`

`make_shade`/`make_light` for groove/ridge/inset/outset rounded borders
always use the generic `r*160/255` darkening formula (and identity for
"light"), but never replicate the legacy near-black special case
(`if ((topBordercolor & 0xFFFFFF) == 0) { shadecolor = 0x4c4c4c; lightcolor
= 0xb2b2b2; }`) used at lines ~10395-10397 etc.

**Failure scenario:** `border-color: black` with `border-style:
groove/ridge/inset/outset` and any `border-radius` computes
`r=g=b=0*160/255=0`, i.e. shade == light == black, so the border renders as
a flat solid black ring with no 3D shading, whereas the equivalent
non-rounded border correctly shows the light/dark bevel via the
`0x4c4c4c`/`0xb2b2b2` override.

## 4. Background-image rounded clip uses mismatched box dimensions (correctness)

**File:** `crengine/src/lvrend.cpp:11052`

In `DrawBackgroundImage`'s rounded-clip setup, the corner radii are computed
from a freshly constructed `RenderRectAccessor fmt(enode)` (the node's full
box), but the clip rect stored in `dei->rounded_clip_rect` is built from this
function's `width`/`height` parameters, which are not always equal to
`fmt`'s dimensions.

**Failure scenario:** The body-background call site
(`DrawBackgroundImage(enode, drawbuf, 0, bg_top, 0, 0, drawbuf.GetWidth(),
drawbuf.GetHeight()-bg_top, false)` at `lvrend.cpp:11239`) passes width/height
derived from the drawbuf/page, not from the body element's
`RenderRectAccessor` box. If `<body>` (or any element reached via a similar
mismatched call site) has `border-radius`, the corner radii are sized and
positioned against the wrong box, so the rounded clip mask is misaligned or
wrongly scaled relative to the actual drawn rect.

## 5. Manually-duplicated struct layout risks silent desync (maintainability)

**File:** `crengine/src/lvdrawbuf.cpp:618`

`RoundedClipShim` hand-mirrors the exact field layout of `draw_extra_info_t`
(declared in `lvrend.h`) instead of including the header, based only on a
comment asserting the two structs are kept in sync.

**Failure scenario:** A future edit to `draw_extra_info_t` in `lvrend.h`
(reordering fields, inserting a new field before `rounded_clip_active`,
changing a type) that isn't mirrored here causes `RoundedClipShim` to read
the wrong bytes as `rounded_clip_active`/`rounded_clip_rect`/`rounded_rx`/
`rounded_ry` with no compiler error, silently feeding garbage bounds into the
(already-vulnerable) blitter loop above (see finding 1).

## 6. Stale comment says dashed rounded borders are unimplemented (comments)

**File:** `crengine/src/lvrend.cpp:10011`

The comment says dashed/dotted rounded borders "fallback to legacy for now"
and are "treated as solid," but the code immediately below (and
`fillRoundedRectDashedRing`, `draw_band_lr_dashed`, `draw_band_tb_dashed`)
fully implements dedicated dashed/dotted rounded-corner rendering rather than
falling back or treating them as solid.

**Failure scenario:** A maintainer reading the comment believes dashed/dotted
rounded borders are unimplemented placeholders and either duplicates the
work or misdiagnoses a dashed-border rendering bug by looking at the legacy
non-rounded path instead of the actual dashed-ring code that runs.

## 7. Ellipse-span math duplicated across 7+ call sites (simplification)

**File:** `crengine/src/lvrend.cpp:420` (and others)

The per-scanline elliptical-corner span computation (compute `xl`/`xr` from
`rx`/`ry`/corner centers via the `dy`/`val`/`dx` formula) is copy-pasted
near-verbatim at least 7 times: `fillRoundedRect`, `fillRoundedRectRing`
(outer span), `fillRoundedRectBorderSidesBand` (outer span),
`computeInnerSpanPerSide` (inner span), `draw_band_lr_dashed`,
`draw_band_tb_dashed`, and again independently in `lvdrawbuf.cpp`'s
`RoundedClipShim` block.

**Failure scenario:** A rounding/off-by-one fix to the ellipse-boundary math
(e.g. the -0.5/+0.5 pixel-center convention) applied to one copy is easy to
miss in the other six, producing visibly inconsistent corner curvature
between the border ring, the background fill, and the background-image clip
mask on the same element.
