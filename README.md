# open-local-services

A local server for image generation, video clips, speech synthesis, transcription, and listening tasks. One server process serves the web UI and all API endpoints from one fixed port. The voice engine is linked into the server. The image engine is served through its own library because it has a separate ggml copy.

## Quick start

```sh
run.bat                 # Windows development build and start
run.sh                  # Linux development build and start
build-rel.bat           # Windows release artifacts
build-rel.sh            # Linux release artifacts
```

The development scripts build missing parts, then start the server. Pass `build` to stop before starting. `run.bat` accepts `vulkan`, `cuda`, `key=`, `rate=`, `arch=`, and `fresh-full-rebuild`. `run.sh` accepts `build`, `key=`, `rate=`, and `fresh-full-rebuild`.

Release builds require committed `api/` and `webui/` changes. They write finished binaries into `Release/<target>/` with `build-info.txt`.

## Access

The server listens on `OLS_PORT`, defaulting to `8765`. If that port is already in use, it refuses to start and does not roll over to another port.

The UI and API use one key from `.env`. Acceptable key formats are:

```http
X-Api-Key: <key>
Authorization: Bearer <key>
GET /path?key=<key>
Cookie: ols-key=<key>
```

A successful API door may set `ols-key`, so later requests do not need to repeat it. If no key is configured, all doors open. After three consecutive failed auth attempts, connections get no response until a 30-second gap passes. API requests use a token bucket with timed refill; refused requests return HTTP 429 with `Retry-After` and do not execute or use the caller's input.

## Configuration

`.env` contains local settings:

```ini
OLS_HOST=0.0.0.0
OLS_PORT=8765
OLS_API_KEY=replace-this
OLS_RATE_LIMIT=600
OLS_THREADS=8
OLS_WEIGHTS_DIR=weights
OLS_DATA_DIR=data
OLS_STARTUP=false
OLS_ELEVATE=off
```

The config panel uses `GET /api/config` and `POST /api/config`. Secret values are shown only as `set` or `unset`.

## Web UI

Pages are embedded in the binary at build time. If a `webui/` folder exists beside the running server, it is served from disk instead.

- `GET /` returns the credential door.
- `/index.html` is the home page.
- `/imagegen.html` is the image studio.
- `/voice.html` is the voice studio.
- `POST /api/webui/expand` writes embedded pages to disk.

## API

Common endpoints:

```text
GET  /api/monitor
GET  /api/system
GET  /api/service
GET  /api/jobs
GET  /api/jobs/<id>
POST /api/jobs/<id>/cancel
```

Image, gallery, reference, and preference features use `/api/` paths. Voice features use `/voice/api/` paths. Generated files use `/files/` and `/voice/files/` paths.
Job endpoints return `202` with `job_id`; jobs are checked with `GET /api/jobs/<id>` or `GET /voice/api/jobs/<id>`.
