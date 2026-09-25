# fitd changes

## 2026-09-24 — Android PWA under path prefix

Optional mount prefix so Tailscale `serve --set-path=/fit` can install Oda Fit as its own PWA (scope `/fit/` only). Unprefixed routes still work when the proxy strips the prefix.

**New flag:** `--base /fit`  
**Env fallback:** `FITD_BASE_PATH`

Normalize: leading slash, no trailing slash; empty (or omitted) = previous root behavior.

Also adds (no auth): `GET /manifest.webmanifest`, `GET /icon-192.png`, `GET /icon-512.png`, `GET /apple-touch-icon.png` (icons read from the directory containing the executable).

## 2026-09-24 — configurable zone and protein hint

- **New flag:** `--tz ZONE` / env `FITD_TZ` — IANA zone for the training day. Default is now the host's `TZ` / system local time instead of a hard-coded zone.
- **New flag:** `--protein-target TEXT` / env `FITD_PROTEIN_TARGET` — optional hint on the protein card. Default: no target shown (previously a hard-coded value).
- Startup log line includes the base path and zone.
- Rest-day template label is plain `Rest`.
