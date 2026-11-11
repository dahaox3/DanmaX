`client.h` is the unmodified mpv v0.41.0 public client API header (ISC license in its header).

Source: https://github.com/mpv-player/mpv/blob/v0.41.0/include/mpv/client.h

SHA-256: `3c073236a09cb456c6e80587d5523c8771c32fb22ca014e0f99cc3d43905b75c`

The Windows XML drop companion uses the documented dynamic cplugin symbol table;
it does not bundle/link a new mpv runtime or use mpv internals. The header is
vendored so configure/build never downloads it. No other third-party code is used.
