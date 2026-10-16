# MSSV FFmpeg canary boundary

This document defines the managed/native FFmpeg process-host boundary used to prepare the first native canary without changing the shipping playback path.

The production-compatible behavior remains the managed `System.Diagnostics.Process` implementation until the native host provides the same stream and lifecycle contract and a canary explicitly selects it.

## Required process-host contract

A process host must provide all of the behavior that `FfmpegProducer` depends on today, not only process creation:

- readable PCM bytes from FFmpeg stdout;
- writable stdin for Icy streams;
- asynchronous stderr lines for duration and diagnostic parsing;
- safe exit-state observation;
- explicit termination;
- cancellation of stderr reading and deterministic disposal.

The managed host in `TS3AudioBot/Audio/FfmpegProcessHost.cs` is the compatibility reference for this boundary.

## Selection and fallback rules

The initial selection contract has two logical backends: `Managed` and `NativeCanary`.

- `Managed` always selects the existing managed behavior.
- `NativeCanary` may be selected only when a native host is actually available.
- If native is requested but unavailable, managed fallback occurs only when fallback is explicitly allowed.
- If native is requested, unavailable, and fallback is disabled, selection fails closed before playback starts.
- A native start must not silently become managed after media output has been committed. Fallback is a pre-start/bootstrap decision, not a mid-stream duplication mechanism.

The current repository only defines and tests this boundary. It does not yet expose a user configuration switch and does not route `FfmpegProducer` through the native backend.

## Canary acceptance gates

Before `NativeCanary` can be exposed in shipping configuration:

1. Extend the native process host so stdin/stdout streaming has parity with the managed host, not only lifecycle supervision.
2. Adapt `FfmpegProducer` to the process-host interface while keeping `Managed` as the unconditional default.
3. Add cross-host tests for normal playback, seek/reconnect behavior, Icy stdin forwarding, stderr duration parsing, cancellation, timeout, and cleanup.
4. Add explicit canary configuration with managed fallback enabled by default for canary instances only.
5. Validate the actual production systemd/cgroup unit policy, resource behavior, and rollback procedure on a bounded canary.
6. Widen rollout only after native/managed parity and resource measurements are accepted.

## Safety boundary

No production credentials, customer media, private topology, or deployment-specific values belong in this public repository. Current host-boundary tests use only local process fixtures and do not change production behavior.
