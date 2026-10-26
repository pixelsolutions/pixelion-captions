# macOS Runtime Validation Matrix (V1-V8)

Feature: macos-platform-support, Task 9.1 (Requirements 9.1-9.9)

This is the runnable manual-validation checklist for the macOS port. The actual
runs are performed by a human on real Macs; this document is the authored
procedure and pass criteria. Every supported combination must pass V1-V8.

## Supported runtime matrix (Req 9.8, 9.9)

| macOS | Architecture | OBS Studio |
|---|---|---|
| 13.7 Ventura | arm64 | 31.1.1 |
| 13.7 Ventura | x86_64 | 31.1.1 |
| 14.x Sonoma | arm64 | 31.1.1 |
| 15.x Sequoia | arm64 | 31.1.1 |

Run the full V1-V8 sequence on each row. Record pass/fail and the build commit
for each row.

## Preconditions

- The signed, notarized `.plugin` bundle installed to
  `~/Library/Application Support/obs-studio/plugins/` (per-user) or
  `/Library/Application Support/obs-studio/plugins/` (system-wide).
- A Pixelion account with available minutes (and a Pro plan for the translate
  checks).
- Working internet connectivity (except where a step says otherwise).

## V1 - 5-minute transcription under Thread Sanitizer (Req 9.1)

1. Launch a TSan-instrumented OBS build (or a TSan plugin build with a TSan OBS).
2. Sign in, start a transcription session on a program-audio source.
3. Run continuously for 5 minutes with steady audio.

Pass: zero ring-buffer overrun, underrun, or data-race reports from TSan during
the 5-minute run.

## V2 - Scene transforms then OBS exit (Req 9.2)

1. With a caption source active, apply scene transforms (move, scale, rotate,
   crop) to the caption/program sources.
2. Confirm the caption renders correctly under each transform.
3. Quit OBS.

Pass: transforms render correctly; OBS shuts down within 10 seconds; no crash on
exit.

## V3 - Optional start/stop with stream/record (Req 9.3)

1. Enable the auto-start-with-stream/record option.
2. Start streaming or recording.

Pass: the live session auto-starts within 5 seconds of stream/record start.

## V4 - Exercise HTTPS and WSS at runtime (Req 9.4)

1. Sign in (exercises HTTPS: OAuth token exchange, `GET /me`, `GET /me/usage`,
   `POST /realtime-token`).
2. Start a session (exercises WSS to `wss://api.openai.com`).

Pass: both HTTPS and WSS connections establish successfully, confirming the
bundled Qt TLS backend works with no user-installed dependency.

## V5 - Start session while host offline (Req 9.5)

1. Disable all network connectivity.
2. Attempt to start a session.

Pass: a clear start-failure message is surfaced via the state callback; the
plugin stays in a usable state (no crash, sign-in state preserved).

## V6 - Network interrupted mid-session (Req 9.6)

1. Start a session with connectivity.
2. Disable connectivity for ~15 seconds mid-session, then restore it.

Pass: the session reconnects and resumes within 30 seconds of connectivity
returning. Text received before the interruption is retained; audio during the
outage is not transcribed (expected).

## V7 - Restart OBS after prior sign-in (Req 9.7)

1. Sign in, then fully quit OBS.
2. Relaunch OBS.

Pass: the Keychain refresh credential persists; the signed-in state is restored
automatically (email and plan shown) without a new browser sign-in.

## V8 - Install notarized bundle, launch OBS (Req 6.5)

1. Install the notarized bundle on a clean Mac (Gatekeeper enabled, no developer
   overrides).
2. Launch OBS.

Pass: the bundle loads with no Gatekeeper block; the plugin appears in the OBS
source/plugin list.

## First-launch permission prompts to expect

- Keychain access prompt when the refresh credential is first saved/read.
- Default browser launch for the OAuth sign-in (no extra prompt, but note it).
- No microphone/camera prompt: the plugin reads the OBS program audio mix, not a
  system input device.
