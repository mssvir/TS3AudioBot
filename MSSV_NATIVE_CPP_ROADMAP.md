# MSSV Native C++ Migration Roadmap

This file is the public, repository-resident roadmap for migrating the MSSV TS3AudioBot fork from the inherited C#/.NET implementation toward a native C++ runtime.

It intentionally contains no private deployment topology, customer configuration, credentials, access tokens, identities, or production-only values.

## End state

The preferred end state is a fully native C++ TS3AudioBot runtime for the feature set required by this fork, with no .NET runtime dependency.

Migration is compatibility-first and incremental. The current C# implementation remains the behavioral reference until each native seam passes automated conformance, failure-mode, resource, canary, and rollback gates. A flag-day rewrite is not an accepted migration strategy.

## Non-negotiable engineering rules

- Preserve the upstream OSL-3.0 license and attribution requirements.
- Keep source changes reviewable and bounded.
- Do not commit secrets, private topology, customer data, or deployment credentials.
- Every native replacement must have deterministic tests before it can replace the managed path.
- Prefer explicit ownership and RAII for processes, sockets, files, buffers, and cancellation state.
- Prefer C++20 or newer where supported by the selected production toolchain.
- Use sanitizer-enabled CI and reproducible Linux builds for native components.
- Language change alone is not success; acceptance requires equal or better correctness, reliability, observability, and resource behavior.

## Phase 0 — freeze the compatibility contract

Before replacing components, capture machine-verifiable behavior for the existing fork:

1. configuration parsing, defaults, upgrades, and required command/plugin behavior;
2. TeamSpeak connection, identity, reconnect/disconnect, and session semantics;
3. audio timing, buffering, Opus behavior, and FFmpeg/external-process behavior;
4. Web/control API requests, responses, authentication/session semantics, and error categories required by integrations;
5. lifecycle behavior for start, clean stop, restart, drain, and child-process cleanup;
6. stable health/status/error signals that can be compared across implementations.

Golden fixtures and conformance tests should live in this repository and be runnable against both the managed reference implementation and native replacements.

## Official upstream modernization bridge

Native migration and managed-runtime modernization are separate tracks.

At the 2026-10-16 recheck, official upstream `Splamy/TS3AudioBot` still has `develop` at `142e4e2fab19f75b8bd00068134992263b6f0c1c`. That reviewed snapshot is substantially ahead of upstream `master` and targets `net10.0`.

After the current reliability baseline is accepted, upstream `develop` should be evaluated before adopting unrelated third-party C# forks. A bounded compatibility branch may port this fork's required fixes and tests onto that upstream line so inherited legacy-runtime risk can be reduced while C++ work proceeds.

This is not a substitute for the current reliability patches: at the reviewed upstream `develop` snapshot, FFmpeg process-start handling still maps a generic `Win32Exception` to a missing-FFmpeg message and does not include this fork's Linux EAGAIN/resource-exhaustion admission behavior. Any move to upstream `develop` therefore requires re-porting and revalidating the fork-specific reliability contract rather than switching bases blindly.

## Phase 1 — native process and FFmpeg supervision

The first preferred native seam is external-process supervision because it has high reliability value and relatively low coupling to TeamSpeak protocol behavior.

The native component should own or mediate:

- child-process admission and bounded concurrency;
- spawn failure classification, including Linux resource exhaustion such as EAGAIN;
- bounded cooldown/backoff and single recovery probes where appropriate;
- process-tree ownership and deterministic cleanup;
- cancellation/timeouts;
- bounded stderr capture/redaction suitable for diagnostics;
- health counters for active children, denied starts, start failures, exits, and cleanup failures.

Acceptance gates:

- behavior matches the existing tested EAGAIN classification/admission contract;
- no orphan FFmpeg children after stop, restart, track change, or failure paths;
- concurrency cannot grow without a configured bound;
- failure paths do not create retry storms;
- process ownership remains friendly to Linux cgroups/systemd;
- rollback to the managed path is immediate and explicit.

### Phase 1 bootstrap status — 2026-10-16

A non-shipping C++20 process-supervision library now defines the first native seam. It implements the managed reference contract for missing-executable classification, Linux EAGAIN/resource-exhaustion classification, bounded exponential cooldown, one half-open recovery probe, success reset, and non-resource probe release. The seam is documented in `native/PROCESS_SUPERVISION_CONTRACT.md`.

The native target is built with CMake + Ninja and is tested under both GCC and Clang with AddressSanitizer and UndefinedBehaviorSanitizer. It is not linked into the shipping TS3AudioBot runtime and therefore does not change production behavior.

The first cross-implementation contract is now machine-readable in `conformance/process_supervision_v1.tsv`. Both the managed C# tests and native C++ tests consume the same classification and admission-policy values, so drift in EAGAIN classification, cooldown timing, half-open probe behavior, success reset, or non-resource failure handling is detected by CI instead of relying on duplicated hand-maintained expectations.

A narrow injectable `ProcessBackend`/`ProcessSupervisor` lifecycle boundary covers spawn, terminate, bounded wait, force-kill, and reap policy. Deterministic failure injection covers EAGAIN, missing executable, generic spawn failure, cancellation, timeout escalation, failed terminate, and incomplete cleanup.

A real Linux backend now implements direct `posix_spawn` execution with no shell interpolation, dedicated process-group ownership, bounded non-reaping exit observation, group SIGTERM/SIGKILL, and final leader reaping. Linux integration fixtures verify real ENOENT classification, dedicated process groups, descendant group inheritance, graceful descendant reaping without an orphan, and timeout escalation against a SIGTERM-ignoring process. This remains non-shipping and is not yet connected to production FFmpeg.

Lifecycle observability is now represented by an optional thread-safe `ProcessSupervisionMetrics` sink. It exposes active children, successful starts, admission denials, categorized start failures, observed exits, forced kills, and cleanup failures without changing existing callers. Unreaped children remain visible as active, and incomplete cleanup is counted explicitly.

The next Phase 1 work is production-shape validation: bounded stderr capture/redaction, cgroup/systemd shutdown verification including stubborn descendants, representative resource measurements, FFmpeg-specific integration fixtures, then a managed-fallback canary.

## Phase 2 — native audio pipeline

Move timing, buffering, Opus/audio flow, and related hot paths behind a stable interface.

Acceptance should include deterministic fixtures where possible, long-running playback tests, stream drop/reconnect behavior, cancellation, leak checks, and CPU/RSS/thread comparisons against the managed reference.

## Phase 3 — native TeamSpeak transport and session layer

Port TeamSpeak transport/session behavior only after packet/session contracts and replay/conformance fixtures exist.

The native implementation must preserve the required identity, connection, reconnect, timeout, crypto/state-machine, voice/audio, and error semantics before it becomes the default path.

## Phase 4 — native control and Web/API surface

Port the remaining runtime control, API, and session behavior while keeping compatibility required by existing integrations.

Authentication/session boundaries must remain explicit. Persistent secrets must never be introduced into URLs, ordinary logs, or source-controlled configuration.

## Phase 5 — native default and .NET retirement

The .NET implementation can be retired only after the native path passes all required compatibility, canary, resource, soak, failure-injection, and rollback tests.

The final release gate should prove that the required service can run entirely from native artifacts without a .NET runtime dependency.

## CI structure

Native work should begin without changing the shipping runtime:

1. add a minimal native build/test target;
2. compile with warnings treated seriously and sanitizer configurations in CI;
3. run native unit tests;
4. run cross-implementation conformance fixtures;
5. publish provenance/checksums for native test artifacts where useful;
6. add benchmarks only after correctness contracts are stable.

Recommended initial toolchain direction:

- CMake + Ninja;
- GCC and Clang coverage on Linux where practical;
- C++20 baseline;
- AddressSanitizer/UndefinedBehaviorSanitizer test jobs;
- ThreadSanitizer for targeted concurrency tests where compatible;
- `clang-tidy`/static analysis as a quality gate once noise is controlled.

Exact dependencies must be selected deliberately and kept minimal.

## Third-party forks

Do not wholesale merge unrelated TS3AudioBot forks.

Potentially useful individual fixes may be reviewed and ported selectively with attribution/license review and tests. A fork being newer or having more commits is not sufficient evidence that it is a safer base.

The authoritative public source remains this repository. Private deployment and customer/runtime state do not belong here.

## Ordered near-term tasks

- [x] Add machine-readable compatibility/conformance fixtures for current C# behavior.
- [ ] Evaluate the reviewed official upstream `develop` modernization line in a separate compatibility track after the current reliability canary is accepted.
- [x] Document the process-supervision seam and its protocol/interface.
- [x] Add a minimal C++ build/test skeleton without changing the shipping runtime.
- [x] Implement native process-spawn classification and bounded admission behavior.
- [x] Add cross-implementation conformance tests for the first seam.
- [x] Add deterministic failure-injection tests for EAGAIN, missing executable, cancellation, timeout, and child cleanup policy.
- [x] Add a real Linux process backend plus process-group/orphan cleanup integration tests.
- [x] Add lifecycle health counters for active children, denied starts, categorized failures, exits, forced kills, and cleanup failures.
- [ ] Add bounded stderr capture/redaction.
- [ ] Verify systemd/cgroup shutdown behavior, including stubborn descendants.
- [ ] Measure CPU, RSS, thread/task count, and child-process behavior under representative playback fan-out.
- [ ] Add FFmpeg-specific native integration fixtures without production/customer data.
- [ ] Canary the native seam behind an explicit fallback path before making it default.
- [ ] Expand migration seam-by-seam only after each prior phase reaches parity.

## Definition of done

Migration is complete when the required TS3AudioBot feature set runs on the native C++ implementation, conformance tests pass, resource/failure behavior is accepted, rollback history is no longer needed for the managed runtime, and the service no longer requires .NET at runtime.
