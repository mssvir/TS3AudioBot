# MSSV TS3AudioBot Production P0

This document is intentionally public-safe. It contains no production hostnames, tokens, credentials, runner secrets, or private infrastructure details.

## Baseline

The current MSSV production-compatibility line is anchored at:

`377f3b7ee3675de1ad6010ced59fba218c6dba95`

The stable base branch for production-compatible patches is:

`mssv/prod-baseline-377f3b7e`

Do not merge production P0 work directly into the fork `master` until the divergent histories have been reconciled and compatibility has been proven separately.

## P0 incident model

Production evidence pointed to repeated FFmpeg child-process startup failures under Linux resource pressure. A native process-start error with `EAGAIN` / errno 11 is resource exhaustion or temporary process-spawn pressure; it is not proof that the FFmpeg executable is missing.

Repeated immediate retries can amplify that pressure. The P0 patch therefore adds a small, bounded admission/backoff gate around FFmpeg process creation and reports the resource-exhaustion case distinctly.

## Compatibility contract

This P0 must not change:

- TeamSpeak wire/protocol behavior;
- existing bot configuration schema;
- existing playback semantics outside FFmpeg process admission;
- existing HLS handling other than additional diagnostics;
- authentication/credentials;
- service lifecycle layout.

The P0 is deliberately not part of the C++ migration.

## Validation

Focused regression coverage verifies:

- missing executable remains a missing-FFmpeg error;
- Linux `EAGAIN` / errno 11 is reported as resource exhaustion;
- unrelated native process errors are not mislabeled;
- admission failures back off in a bounded way;
- a successful process start resets the failure state;
- one probe is admitted after cooldown.

The final P0 branch workflow is read-only (`contents: read`) and runs the focused tests on every relevant push/PR.

A previous broad test run passed 73 of 75 tests. The two failures were YouTube integration tests on the hosted runner where `youtube-dl`/`yt-dlp` was not installed; the P0 tests passed.

## Production rollout contract

Production deployment belongs in the private MSSV control/deployment repository, not in this public fork.

Required order:

1. build the reviewed production-compatible commit;
2. preserve the currently deployed known-good release for rollback;
3. deploy to the smallest safe canary scope first;
4. verify process-start errors, FFmpeg child count, reconnect rate, playback probes, CPU/memory/PID pressure, and service health;
5. expand only if the canary remains healthy;
6. roll back immediately on playback, reconnect, or resource-pressure regression.

No production credential may be passed through command-line logs or committed files.

## Rollback

Rollback means switching the private production deployment back to the previously verified release/commit and re-running health checks. Do not attempt to compensate for a failed P0 rollout with additional live source edits.

## Follow-up work

Keep these separate from P0:

- reconciliation of the production baseline with the fork `master` and current upstream;
- lifecycle/control-plane automation;
- SSO and UI integration;
- staged C#/.NET to C++ migration;
- .NET support-lifecycle upgrade;
- dependency/security-advisory remediation.
