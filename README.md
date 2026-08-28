# Oda Fit (`fitd`)

A private gym logger. One small C binary serves a phone page over Tailscale and writes the same SQLite ledger a local `fit` CLI already uses.

No cloud. No accounts. No second database. The overlay network is the auth.

## What you get

- Today's rotation, planned sets, and last-session working-weight targets
- Log a set as you go (reps, weight, optional drop-set note)
- Protein is a separate card, often later the same night — saving sets never requires it
- Rest days show one line, not fake boxes

The page is phone-first. Dark ink, gold, crimson. Bind Tailscale IPv4 only; `0.0.0.0` is refused.

## Build

Needs `gcc` and `libsqlite3-dev`.

```
make
./fitd
```

Flags:

| | default |
|---|---|
| `--bind IPV4` | `tailscale ip -4`, else `127.0.0.1` |
| `--port N` | `9120` |
| `--db PATH` | `$HOME/.hermes/profiles/fitness/fitness.db` |

If Tailscale is down it binds localhost and fails loud if the database cannot be opened. It does not invent lifts or protein.

## HTTP

| | |
|---|---|
| `GET /` | HTML app |
| `GET /api/today` | plan, targets, logged sets, latest protein |
| `POST /set` | form or JSON `{exercise, reps, weight?, note?}` — always appends |
| `POST /protein` or `POST /api/protein` | form or JSON `{value, date?}` |
| `GET /health` | `ok` |

Dates use `America/Phoenix` so a midnight UTC clock cannot shift the training day. Rotation and `set_n` match the `fit` CLI: Monday fasting rest and Tuesday legs do not move; the second rest day floats via `meta.float_rest`.

## systemd (user unit)

Point `ExecStart` at the binary. Do not pass `--bind 0.0.0.0`. After `tailscaled.service`. This is a tiny extra listener — leave whatever else you run alone.

## License

Source is here so other people can run the same idea on their own tailnet. Keep your numbers and your IPs off the internet.
