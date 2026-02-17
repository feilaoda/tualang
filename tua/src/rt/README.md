# tua_rt (C runtime)

## Goals
- Keep `std/` mostly in Tua; keep C runtime small and stable.
- Unix first (Linux/macOS); Windows later with the same API surface.

## Conventions
- Paths passed to `tua_rt` APIs are UTF-8.
- `tua_err_t` is a stable, cross-platform error code (`TUA_E_*`), not raw `errno`.

