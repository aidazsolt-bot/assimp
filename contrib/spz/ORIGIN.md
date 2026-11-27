# contrib/spz

Vendored C++ decoder from https://github.com/nianticlabs/spz (MIT).

Copied paths:

- `src/cc/*` (load-spz, splat-types, splat-c-types, splat-utils)
- `LICENSE`, `README.md` (upstream)

Extensions / WASM / Python / CLI tools are not vendored. Assimp links
system `libzstd` (+ Assimp zlib) for NGSP v4 and legacy gzip SPZ.
