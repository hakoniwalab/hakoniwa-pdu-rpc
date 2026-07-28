# Hakoniwa PDU RPC Test Contract

## Purpose

This document defines what the `hakoniwa-pdu-rpc` tests are intended to guarantee.

The key rule is that a failing test is not, by itself, proof that production RPC code is wrong. Before changing production code, confirm that the test still represents the current RPC contract.

The current production baseline is the RPC implementation validated through `hakoniwa-conductor-light` on Ubuntu x64, Ubuntu ARM64, macOS, and Windows x64. The Conductor Light integration tests exercise the RPC layer through real higher-level use cases, including timeout/cancel races, TCP lifecycle, reconnect, and multi-client behavior.

## Test layers

The RPC test strategy is split into three layers.

### 1. Package/build contract

Purpose: prove that PDU RPC is consumable as a cross-platform CMake component.

This layer is independent from RPC runtime semantics.

Required checks:

```text
python -m unittest tools/test_hako.py -v
python tools/hako.py doctor
python tools/hako.py configure --dry-run
python tools/hako.py build
python tools/hako.py install
python tools/hako.py package-test
```

The package/build contract is executed with both the repository default
`hakoniwa-build.yaml` and `test/fixtures/alternate-build.yaml`. The manifest
resolver tests additionally fix CLI/manifest/environment precedence and the
operation-specific tests ON/OFF behavior.

Supported CI platforms:

- Ubuntu x64
- Ubuntu ARM64
- macOS
- Windows x64

A failure in this layer should be treated as a build/package/platform problem, not as an RPC semantic failure.

### 2. PDU-RPC contract tests

Purpose: verify the state-machine and request/response contract owned by `hakoniwa-pdu-rpc` itself.

These tests should be small, explicit, and avoid assumptions about higher-level Conductor behavior.

### 3. Higher-level integration tests

Purpose: verify that PDU RPC works correctly when used by a real consumer.

`hakoniwa-conductor-light` currently provides this layer and covers behavior that should not be duplicated unnecessarily in the PDU-RPC repository.

Examples include:

- repeated RPC calls through real Conductor services;
- timeout followed by explicit cancellation;
- normal-response/cancel race handling;
- client/server lifecycle;
- TCP disconnect and reconnect;
- multiple clients;
- `max_clients` enforcement.

## RPC timeout/cancel contract

The RPC lifecycle follows the Hakoniwa Core semantics.

```text
REQUEST
  -> RUNNING
  -> timeout event
  -> caller explicitly sends CANCEL
  -> CANCELLING
  -> either:
       normal response wins -> RESPONSE_IN -> IDLE
       cancel completion wins -> RESPONSE_CANCEL -> IDLE
```

Important invariants:

1. `RESPONSE_TIMEOUT` is a notification, not completion of the RPC.
2. Timeout does not implicitly cancel the request.
3. The caller owns the decision to call `send_cancel_request()`.
4. After cancellation is sent, a normal response may still win the race.
5. The endpoint becomes reusable only after a terminal normal/cancel response resolves the in-flight request.
6. Server failure is not inferred from RPC timeout alone.

## Current PDU-RPC tests

### `ConfigParsingTest`

Current behavior:

```text
initialize server/client
  -> send Add(5, 7)
  -> server receives request
  -> server replies
  -> client receives RESPONSE_IN
  -> verify sum == 12
```

Assessment: **valid, but misleadingly named**.

The test is primarily a basic request/response round-trip test, not merely configuration parsing.

Recommended direction:

- retain the scenario;
- rename to something such as `BasicRoundTripTest` when test cleanup is performed.

### `MultipleServiceCallsTest`

Current behavior:

```text
request 1 -> response 1
request 2 -> response 2
```

Assessment: **valid**.

Purpose: verify that a completed RPC endpoint returns to an idle/reusable state and supports consecutive calls.

### `RpcCallInfiniteWaitTest`

Current behavior:

```text
call(timeout = 0)
  -> server deliberately delays response
  -> client must not report RESPONSE_TIMEOUT
  -> normal response arrives
```

Assessment: **semantically valid; timing-sensitive implementation should be reviewed**.

The requirement is useful: timeout value `0` means infinite wait. However, fixed sleeps and polling windows should not become accidental platform-performance requirements.

### `RpcCallTimeoutTest`

Current behavior:

```text
request
  -> server receives request but does not reply
  -> client observes RESPONSE_TIMEOUT
  -> test tears down endpoints/services
```

Assessment: **requires review**.

The observation of `RESPONSE_TIMEOUT` is valid. The teardown pattern may not reflect the current contract because the request remains in `RUNNING` after timeout.

Questions to resolve before modifying this test:

- Is the intended requirement only "timeout notification is emitted"?
- Should the test explicitly resolve the in-flight request before teardown?
- Is this scenario redundant with `TimeoutRequiresExplicitCancelAndReturnsToIdle`?

Do not change production RPC code merely to make this legacy cleanup pattern pass.

### `TimeoutRequiresExplicitCancelAndReturnsToIdle`

Current behavior:

```text
REQUEST
  -> RESPONSE_TIMEOUT
  -> explicit send_cancel_request()
  -> server receives REQUEST_CANCEL
  -> server returns canceled response
  -> client receives RESPONSE_CANCEL
  -> next RPC succeeds
```

Assessment: **valid and important**.

This is the primary PDU-RPC state-machine regression for timeout/cancel behavior.

### `NormalResponseMayWinRaceAfterTimeout`

Current behavior:

```text
REQUEST
  -> RESPONSE_TIMEOUT
  -> server sends normal response
  -> test expects RESPONSE_IN
```

Assessment: **current pattern is questionable**.

The test name describes a real requirement, but the current sequence does not issue explicit cancellation after the timeout.

The race that needs protection is:

```text
REQUEST
  -> RESPONSE_TIMEOUT
  -> caller sends CANCEL
  -> server normal response completes before CANCEL wins
  -> client receives RESPONSE_IN
  -> late CANCEL is harmless
```

This is the pattern already exercised by `hakoniwa-conductor-light` and is the appropriate candidate for a focused PDU-RPC regression.

The test should be reviewed and corrected before treating a failure as evidence of a production-code bug.

## Comparison with Conductor Light

Conductor Light is a higher-level consumer and therefore provides stronger end-to-end evidence than isolated PDU-RPC unit/integration fixtures for several scenarios.

### Timeout/cancel race

Conductor Light deliberately pauses server request dispatch while keeping the transport and subscriptions alive:

```text
server poller stopped
  -> client RPC times out
  -> Conductor sends explicit CANCEL
  -> server poller resumes
  -> request/cancel race resolves
  -> normal response or cancel completion terminates the request
```

This is a valid representation of the production race because the server remains alive and both request and cancel can still be delivered.

### Lifecycle and reconnect

Conductor Light also verifies:

```text
connect
  -> attach
  -> disconnect
  -> remaining client continues
  -> reconnect
  -> reattach with new session
```

These behaviors belong primarily to the higher-level transport/integration layer and do not need to be duplicated in every low-level PDU-RPC test.

### `max_clients`

Conductor Light verifies `max_clients` at the TCP connection boundary.

A rejected client has no server-side RPC peer. Therefore issuing an RPC from that intentionally rejected connection is not a valid way to test `max_clients`; it instead creates an unresolved timeout/cancel lifecycle with no peer available to complete it.

## Test review policy

When a test fails, use this order:

```text
1. Identify the requirement the test claims to protect.
2. Confirm that requirement against the current RPC contract.
3. Check whether the test sequence actually represents that requirement.
4. Check whether timing/platform assumptions are unnecessarily strict.
5. Only then decide whether production code is wrong.
```

Possible outcomes:

### Test is invalid or obsolete

- modify or remove the test;
- document the reason;
- do not change production behavior to preserve an obsolete assumption.

### Test requirement is valid, but the test pattern is wrong

- keep the requirement;
- rewrite the fixture/scenario to exercise the real state transition;
- prefer deterministic state/event synchronization over arbitrary sleeps.

### Test and pattern are valid, but failure is timing/platform dependent

- open a focused issue with the failing platform, scenario, and reproduction;
- keep the production behavior unchanged until root cause is understood;
- fix timing, transport, or synchronization independently.

### Production behavior violates a confirmed contract

- fix production code;
- add or retain the smallest regression test that captures the invariant;
- verify Conductor Light integration remains green.

## Proposed test ownership

```text
PDU-RPC
  BasicRoundTrip
  MultipleCalls
  InfiniteWait
  TimeoutRequiresExplicitCancelAndReturnsToIdle
  NormalResponseMayWinCancelRace

Conductor Light
  higher-level timeout/cancel integration
  RPC service composition
  TCP lifecycle
  disconnect/reconnect
  multi-client
  max_clients
  simulation control/event workflows
```

The goal is not maximum test duplication. The goal is clear ownership of each invariant and confidence that a test failure means something precise.
