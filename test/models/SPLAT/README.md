# SPLAT fixtures (antimatter15)

| File | Notes |
|---|---|
| `tiny_dc.splat` | 2 gaussians, hand-built for unit tests (64 bytes) |

Format: headerless little-endian 32-byte records
(`pos` 3xf32, `scale` 3xf32, `RGBA` 4xu8, `rot` 4xu8 wxyz).

See `doc/GaussianSplatting.md`.
