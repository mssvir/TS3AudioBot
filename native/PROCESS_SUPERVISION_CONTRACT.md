# MSSV native process-supervision contract

This directory begins Phase 1 of the MSSV native C++ migration. It is intentionally non-shipping: the production TS3AudioBot runtime still uses the managed implementation until explicit parity, failure-mode, Linux integration, observability, resource, canary, and rollback gates are satisfied.

## First seam

The first native seam covers process-start classification, admission policy, lifecycle observability, and a narrow child-process lifecycle contract. It includes a real Linux backend for integration testing, but it does not alter the shipping TS3AudioBot runtime and does not yet supervise production FFmpeg processes.

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
- Linux process creation uses direct argv execution rather than shell interpolation;
- every successfully spawned Linux child leads a dedicated process group owned by the backend;
- terminate and force-kill signals target the owned process group, not only the leader PID;
- backend destruction performs best-effort force cleanup of any still-tracked children;
- injected backend failures must be testable without production credentials, customer data, mutable production state, or live FFmpeg children.

## Injectable lifecycle boundary

`ProcessBackend` is the narrow ownership boundary for the native seam. It exposes only spawn, terminate, bounded wait, force-kill, and reap operations. `ProcessSupervisor` composes that backend with the admission gate, error classifier, and an optional metrics sink.

`LinuxProcessBackend` is the first real backend. It uses `posix_spawn` with `POSIX_SPAWN_SETPGROUP`, inherits the current environment, constructs argv directly, observes exit without prematurely reaping via `waitid(..., WNOWAIT)`, and performs the final leader reap with `waitpid`. Child ownership is reserved before spawn so a successful spawn cannot become untracked because of a post-spawn container allocation.

## Lifecycle observability

`ProcessSupervisionMetrics` is a native, thread-safe in-memory metrics surface. The supervisor can update it without changing existing callers because the metrics sink is optional.

The current snapshot exposes:

- active children;
- total successful starts;
- admission-denied starts;
- total start failures;
- resource-exhaustion start failures;
- missing-executable failures;
- other native start failures;
- observed exits;
- successful forced-kill requests;
- incomplete cleanup failures.

Counters use relaxed atomics because they are observability signals rather than synchronization primitives. Active-child decrement is saturating, so an unexpected reap event cannot wrap the gauge below zero. An unreaped child remains visible as active and incomplete cleanup increments the cleanup-failure counter.

## Safety boundary

The native library remains non-shipping. Nothing in this directory is linked into the shipping TS3AudioBot executable, and CI must not require production credentials, customer configuration, private topology, or mutable production data.

Passing the current Linux tests proves the lifecycle mechanics exercised by the fixtures; it does **not** yet prove production FFmpeg compatibility, stderr capture behavior, cgroup/systemd shutdown behavior, resource limits, long-running playback behavior, or safe canary rollback.

## Failure-injection coverage

Deterministic native tests currently inject:

- Linux EAGAIN/resource exhaustion and admission cooldown;
- missing executable/native error 2 without resource cooldown;
- generic native spawn failure after a half-open probe;
- graceful cancellation cleanup;
- timeout escalation to force-kill and reap;
- failed cleanup where exit/reap cannot be confirmed;
- failed terminate request followed by deterministic escalation;
- metrics behavior for successful lifecycle, resource pressure plus admission denial, categorized start failures, and incomplete forced cleanup.

## Linux integration coverage

Real Linux integration tests verify:

- a missing executable is surfaced as real `ENOENT` through the backend;
- a spawned child is the leader of its own process group;
- a descendant inherits that supervised process group;
- graceful group termination allows the fixture leader to reap its descendant and leaves no orphan descendant behind;
- a SIGTERM-ignoring process exceeds the grace interval and is then terminated with process-group SIGKILL;
- successful cleanup removes backend ownership and the process group disappears.

The process-tree fixture is test-only and contains no production data or credentials.

## Next acceptance work

Before this seam may supervise real FFmpeg children in a canary, add:

1. bounded stderr capture/redaction;
2. systemd/cgroup-friendly ownership and shutdown verification, including stubborn descendant behavior;
3. representative CPU, RSS, task/thread, and child-process measurements;
4. FFmpeg-specific integration coverage using non-sensitive fixtures;
5. canary integration behind an explicit managed fallback.
