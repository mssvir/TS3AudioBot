# MSSV native process-supervision contract

This directory begins Phase 1 of the MSSV native C++ migration. It is intentionally non-shipping: the production TS3AudioBot runtime still uses the managed implementation until explicit parity, failure-mode, Linux integration, canary, and rollback gates are satisfied.

## First seam

The first native seam covers process-start classification, admission policy, and a narrow injectable child-process lifecycle contract. It does not execute a native Linux process backend yet and does not alter TeamSpeak, configuration, audio, Web/API, database, authentication, or deployment behavior.

The managed reference behavior is `TS3AudioBot/Audio/FfmpegStartAdmissionGate.cs` plus the FFmpeg process-start classification in `TS3AudioBot/Audio/FfmpegProducer.cs`. Shared classification/admission expectations live in `conformance/process_supervision_v1.tsv` and are consumed by both managed and native tests.

The native contract currently requires:

- native error 2 remains the missing-executable category;
- Linux errno 11/EAGAIN is resource exhaustion, not a missing binary;
- unrelated native errors stay a separate category;
- before resource pressure, starts are admitted normally;
- the first resource-exhaustion failure opens a 250 ms cooldown;
- repeated resource failures use bounded exponential cooldown: 250 ms, 500 ms, 1 s, 2 s, 4 s, then 5 s maximum;
- after cooldown, only one half-open recovery probe is admitted;
- a successful process start resets the resource-pressure state;
- a non-resource failure releases the half-open probe but does not erase prior resource-pressure state;
- cancellation and timeout cleanup request termination first, wait for a bounded grace interval, escalate to force-kill if exit is not observed, then reap;
- cleanup failure must remain visible and must never be reported as complete;
- injected backend failures must be testable without production credentials, customer data, mutable production state, or live FFmpeg children.

## Injectable lifecycle boundary

`ProcessBackend` is the narrow ownership boundary for the next native stage. It exposes only spawn, terminate, bounded wait, force-kill, and reap operations. `ProcessSupervisor` composes that backend with the admission gate and error classifier.

The current backend used by failure-injection tests is deterministic and fake. This deliberately validates lifecycle policy before adding Linux syscalls. Passing these tests does **not** yet prove real process-group ownership, orphan prevention, systemd/cgroup behavior, or FFmpeg integration.

## Safety boundary

The native library remains non-shipping. Nothing in this directory is linked into the shipping TS3AudioBot executable, and CI must not require production credentials, customer configuration, private topology, or mutable production data.

## Failure-injection coverage

Deterministic native tests currently inject:

- Linux EAGAIN/resource exhaustion and admission cooldown;
- missing executable/native error 2 without resource cooldown;
- generic native spawn failure after a half-open probe;
- graceful cancellation cleanup;
- timeout escalation to force-kill and reap;
- failed cleanup where exit/reap cannot be confirmed;
- failed terminate request followed by deterministic escalation.

## Next acceptance work

Before this seam may supervise real FFmpeg children, add:

1. a Linux backend with explicit process-group ownership and no shell interpolation;
2. Linux integration tests for real spawn, timeout, cancellation, process-tree cleanup, and orphan detection;
3. bounded stderr capture/redaction;
4. counters for active children, denied starts, failures, exits, forced kills, and cleanup failures;
5. systemd/cgroup-friendly ownership and shutdown verification;
6. representative CPU, RSS, task/thread, and child-process measurements;
7. canary integration behind an explicit managed fallback.
