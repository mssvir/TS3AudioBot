# MSSV native process-supervision contract

This directory begins Phase 1 of the MSSV native C++ migration. It is intentionally non-shipping: the production TS3AudioBot runtime still uses the managed implementation until explicit parity, failure-mode, canary, and rollback gates are satisfied.

## First seam

The first native seam covers only process-start classification and admission policy. It does not spawn FFmpeg yet and does not alter TeamSpeak, configuration, audio, Web/API, database, authentication, or deployment behavior.

The managed reference behavior is `TS3AudioBot/Audio/FfmpegStartAdmissionGate.cs` plus the FFmpeg process-start classification in `TS3AudioBot/Audio/FfmpegProducer.cs`.

The native contract currently requires:

- native error 2 remains the missing-executable category;
- Linux errno 11/EAGAIN is resource exhaustion, not a missing binary;
- unrelated native errors stay a separate category;
- before resource pressure, starts are admitted normally;
- the first resource-exhaustion failure opens a 250 ms cooldown;
- repeated resource failures use bounded exponential cooldown: 250 ms, 500 ms, 1 s, 2 s, 4 s, then 5 s maximum;
- after cooldown, only one half-open recovery probe is admitted;
- a successful process start resets the resource-pressure state;
- a non-resource failure releases the half-open probe but does not erase prior resource-pressure state.

## Safety boundary

The native library is test-only at this stage. Nothing in this directory is linked into the shipping TS3AudioBot executable, and CI must not require production credentials, customer configuration, private topology, or mutable production data.

## Next acceptance work

Before this seam may supervise real FFmpeg children, add:

1. a narrow process-spawn interface with deterministic ownership;
2. timeout and cancellation semantics;
3. process-tree cleanup and orphan detection;
4. bounded stderr capture/redaction;
5. counters for active children, denied starts, failures, exits, and cleanup failures;
6. cross-implementation fixture execution against managed and native paths;
7. Linux failure injection for EAGAIN, missing executable, timeout, cancellation, and cleanup;
8. canary integration behind an explicit managed fallback.
