# MSSV native process-supervision contract

This directory begins Phase 1 of the MSSV native C++ migration. It is intentionally non-shipping: the production TS3AudioBot runtime still uses the managed implementation until explicit parity, failure-mode, Linux integration, observability, resource, canary, and rollback gates are satisfied.

## First seam

The first native seam covers process-start classification, admission policy, lifecycle observability, bounded diagnostic stderr, and a narrow child-process lifecycle contract. It includes a real Linux backend for integration testing, but it does not alter the shipping TS3AudioBot runtime and does not yet supervise production FFmpeg processes.

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
- cancellation and timeout cleanup request termination first, wait for a bounded grace interval, escalate to force-kill if the owned process group is not quiescent, then finalize ownership;
- cleanup failure must remain visible and must never be reported as complete;
- Linux process creation uses direct argv execution rather than shell interpolation;
- every successfully spawned Linux child leads a dedicated process group owned by the backend;
- terminate and force-kill signals target the owned process group, not only the leader PID;
- leader exit alone is not sufficient for successful cleanup while descendants remain in the owned process group;
- backend destruction performs best-effort force cleanup of any still-tracked children and groups;
- injected backend failures must be testable without production credentials, customer data, mutable production state, or live FFmpeg children.

## Injectable lifecycle boundary

`ProcessBackend` is the narrow ownership boundary for the native seam. It exposes only spawn, terminate, bounded wait, force-kill, and reap/finalization operations. `ProcessSupervisor` composes that backend with the admission gate, error classifier, and an optional metrics sink.

`LinuxProcessBackend` is the first real backend. It uses `posix_spawn` with `POSIX_SPAWN_SETPGROUP`, inherits the current environment, and constructs argv directly. The Linux implementation treats `wait_for_exit` as a stronger ownership check: it opportunistically reaps the direct leader with `waitpid(..., WNOHANG)` once the leader exits, but does not report success until `kill(-pgid, 0)` confirms that the owned process group no longer exists. If a descendant survives the grace interval, `ProcessSupervisor` therefore escalates to process-group SIGKILL instead of falsely reporting graceful cleanup. `reap` finalizes tracked ownership only after both leader reaping and process-group quiescence are established.

Child tracking capacity and the tracked-child object are prepared before spawn so a successful child cannot become untracked because of a post-spawn container allocation.

## Lifecycle observability

`ProcessSupervisionMetrics` is a native, thread-safe in-memory metrics surface. The supervisor can update it without changing existing callers because the metrics sink is optional.

The current snapshot exposes active children, successful starts, admission-denied starts, total and categorized start failures, observed exits, successful forced-kill requests, and incomplete cleanup failures.

Counters use relaxed atomics because they are observability signals rather than synchronization primitives. Active-child decrement is saturating, so an unexpected finalization event cannot wrap the gauge below zero. An unfinalized child/group remains visible as active and incomplete cleanup increments the cleanup-failure counter.

## Bounded stderr diagnostics

`LinuxProcessBackend` can optionally capture child stderr for diagnostics. Capture is disabled unless `ProcessSpec::stderr_capture` is configured.

The diagnostic contract is deliberately fail-closed and bounded:

- capture uses a dedicated pipe and background drain thread, so storing only a small diagnostic prefix does not allow a noisy child to block after filling the pipe;
- exposed text is capped at the configured byte limit, with a hard implementation ceiling of 64 KiB per child;
- at most 32 explicit literal redaction values are accepted and each literal is capped at 4096 bytes;
- configured literals are replaced before any diagnostic snapshot is exposed;
- if truncation cuts through a configured secret literal, the matching partial suffix is removed rather than exposed;
- invalid capture bounds/configuration fail before the child is spawned;
- pipe descriptors are kept above stdin/stdout/stderr before `posix_spawn` file actions are installed, avoiding accidental standard-stream descriptor aliasing;
- the child receives only the pipe write end as stderr; the parent closes its write copy after spawn;
- raw captured text remains private to the backend and the public snapshot API returns only redacted text plus `truncated` and `complete` state.

This is diagnostic plumbing, not a general logging subsystem. No command-line arguments, environment values, credentials, or customer data are automatically emitted by this code.

## Safety boundary

The native library remains non-shipping. Nothing in this directory is linked into the shipping TS3AudioBot executable, and CI must not require production credentials, customer configuration, private topology, or mutable production data.

Passing the current Linux tests proves the lifecycle and diagnostic mechanics exercised by the fixtures; it does **not** yet prove production FFmpeg compatibility, real systemd/cgroup unit behavior, behavior of descendants that deliberately escape the owned process group/session, resource limits, long-running playback behavior, or safe canary rollback.

## Failure-injection coverage

Deterministic native tests cover Linux EAGAIN/resource exhaustion and admission cooldown, missing executable/native error 2, generic native spawn failure, graceful cancellation, timeout escalation, failed cleanup, failed terminate, and lifecycle metrics behavior.

## Linux integration coverage

Real Linux integration tests verify:

- a missing executable is surfaced as real `ENOENT` through the backend;
- a spawned child is the leader of its own process group and descendants inherit that group;
- graceful group termination leaves no tested orphan descendant behind;
- a leader that exits after SIGTERM does not make cleanup succeed while a descendant in the same process group deliberately ignores SIGTERM;
- such a stubborn descendant forces process-group SIGKILL escalation before cleanup can complete;
- a SIGTERM-ignoring leader is escalated to process-group SIGKILL;
- successful cleanup removes backend ownership and the process group disappears;
- invalid stderr capture configuration fails before spawn;
- a fixture can emit more than the pipe capacity without deadlocking because stderr is continuously drained;
- exposed stderr stays within its configured bound and reports truncation;
- configured literal secrets are redacted even when emitted across separate writes;
- a secret prefix cut by the capture boundary is dropped rather than exposed.

The process-tree/stderr fixture is test-only and contains no production data or credentials.

## Next acceptance work

Before this seam may supervise real FFmpeg children in a canary, add:

1. controlled systemd/cgroup unit shutdown verification, including cgroup cleanup and descendants that change session/process-group membership;
2. representative CPU, RSS, task/thread, and child-process measurements;
3. FFmpeg-specific integration coverage using non-sensitive fixtures;
4. canary integration behind an explicit managed fallback.
