# Heater Relay Control – Specification

## Overview

Control system for two 1 kW resistive heating elements (R1, R2 @ 230 V AC)
using three SPDT relays (A, B, C) to achieve four power levels:
**0 W / 0.5 kW / 1 kW / 2 kW**.

## Wiring

Each relay has three terminals: `COM`, `NO` (normally open), `NC` (normally closed).
When relay is energized (state = 1): `COM` connects to `NO`.
When relay is de-energized (state = 0): `COM` connects to `NC`.

### Fixed connections

| From         | To                                  |
|--------------|-------------------------------------|
| `L` (mains)  | `A.COM`                             |
| `N` (mains)  | `B.COM`                             |
| `C.COM`      | `R1.top`                            |
| `R1.bottom`  | `R2.top` (direct wire, node `Y`)    |
| `B.NO`       | node `Y` (between R1 and R2)        |
| `C.NO`       | node `X` (= `A.NO` = `R2.bottom`)   |
| `A.NO`       | node `X`                            |
| `R2.bottom`  | node `X`                            |
| `C.NC`       | `B.NC` (node `Z`)                   |
| `B.NC`       | node `Z`                            |
| `A.NC`       | unused (leave disconnected)         |

### Nodes summary

- **X** = `C.NO` + `A.NO` + `R2.bottom`
- **Y** = `B.NO` + `R1.bottom` + `R2.top`
- **Z** = `C.NC` + `B.NC`

## State table

State is encoded as `(A, B, C)`. Power dissipated by R1 + R2 in watts.

| A | B | C | Power   | Active path                                          |
|---|---|---|---------|------------------------------------------------------|
| 0 | 0 | 0 | 0 W     | L disconnected (A.NO open)                           |
| 1 | 0 | 0 | 500 W   | L → A.NO → R2 → Y → R1 → C.COM → C.NC → B.NC → N (R1+R2 in **series**) |
| 0 | 1 | 0 | 0 W     | L disconnected                                       |
| 1 | 1 | 0 | 1000 W  | L → A.NO → R2 → Y → B.NO → N (only R2 active)        |
| 0 | 0 | 1 | 0 W     | L disconnected                                       |
| 1 | 0 | 1 | 0 W     | L on both ends of nothing useful (N isolated at Z)   |
| 0 | 1 | 1 | 0 W     | L disconnected                                       |
| 1 | 1 | 1 | 2000 W  | R1 and R2 both directly across L–N (**parallel**)    |

### Canonical states

| Power     | A | B | C |
|-----------|---|---|---|
| OFF       | 0 | 0 | 0 |
| 0.5 kW    | 1 | 0 | 0 |
| 1 kW      | 1 | 1 | 0 |
| 2 kW      | 1 | 1 | 1 |

## ⚠️ Critical safety constraint: short-circuit risk

There is exactly one dangerous condition during transitions:

> **Switching relay C while `B == 0` AND `A == 1` causes an L–N short circuit
> through relay C's contact arc.**

Mechanism: during C's transition, `C.COM` is briefly bridged between `C.NO` and `C.NC`
via the moving contact / arc. With `A == 1`, `C.NO` (= node X) is at potential L.
With `B == 0`, `C.NC` (= node Z) is at potential N (via B.NC → B.COM = N).
Result: L–N short across C's contacts → high fault current, contact welding,
arc damage, possible fire.

### Safe-transition rule

**Before toggling C, ensure `B == 1` OR `A == 0`.**

Equivalent formulation: C may only change state when at least one of these holds:
- `A == 0` (mains L is disconnected at A, no path can short)
- `B == 1` (N is routed to node Y via B.NO, not to Z via B.NC)

### Other transitions (all safe)

- **Toggling B** (any A, C): no L–N short possible.
- **Toggling A** (any B, C): A has no NC connection, so no transition arc creates a short.

## Recommended state-transition graph

Use only these edges between canonical states. Each edge represents toggling **one** relay.

```
OFF (0,0,0) ──[A: 0→1]──> 0.5 kW (1,0,0) ──[B: 0→1]──> 1 kW (1,1,0) ──[C: 0→1]──> 2 kW (1,1,1)
  ▲                            ▲                            ▲                            │
  │                            │                            │                            │
  └──[A: 1→0]──────────────────┴──[B: 1→0]──────────────────┴──[C: 1→0]─────────────────┘
```

**Forbidden direct transitions** (would require simultaneous toggle of C and B,
or pass through the dangerous C-toggle condition):

- `0.5 kW ↔ 2 kW` directly → always go via 1 kW
- `OFF → 1 kW` directly → go via 0.5 kW (only toggles A, then B; both safe)
- `OFF → 2 kW` directly → go via 0.5 kW → 1 kW → 2 kW

### Going to OFF

Always set `A = 0` first, then optionally reset C and B. With `A = 0` no current
flows regardless of C and B states, so resetting C while B = 0 is safe (the
dangerous condition requires `A = 1`).

## Implementation notes

### Required inter-step delay

After each relay toggle, wait for the relay to settle before issuing the next
command. Typical mechanical relay operate/release time: 5–15 ms. Use a
conservative **delay of at least 30 ms** between consecutive toggles.

### State machine API (suggested)

```python
class HeaterController:
    OFF      = (0, 0, 0)
    P_500W   = (1, 0, 0)
    P_1000W  = (1, 1, 0)
    P_2000W  = (1, 1, 1)

    # Adjacency list of safe single-relay transitions
    _NEIGHBORS = {
        OFF:     [P_500W],
        P_500W:  [OFF, P_1000W],
        P_1000W: [P_500W, P_2000W],
        P_2000W: [P_1000W],
    }

    def set_power(self, target):
        """Walk the state graph from current to target, one relay at a time."""
        path = self._shortest_path(self._current, target)
        for next_state in path[1:]:
            self._apply(next_state)
            time.sleep(0.030)
        self._current = target
```

The graph is a simple chain `OFF — 0.5 — 1 — 2`, so `_shortest_path` is trivial:
walk up or down the chain.

### Hardware safeguards (in addition to software)

- **Mains breaker** on L: type B, 16 A (limits fault current if a short occurs
  due to bug or relay failure).
- **Relay rating**: minimum 10 A @ 250 V AC for the 2 kW parallel mode (8.7 A
  nominal). Prefer 16 A relays for headroom.
- **Snubber / RC across each relay contact** is optional for resistive load
  but reduces arc wear (e.g., 100 nF + 100 Ω in series).
- **Fuse on L** sized for 10 A (slow-blow) gives faster fault clearance than
  the breaker alone.

### State persistence

On controller startup, the actual relay state is unknown. Drive all three
relays to the OFF state (`A=0, B=0, C=0`) before assuming `current = OFF`.
The OFF→OFF "transition" is a no-op so it is safe to issue regardless of
prior state.

### Logic-level wiring (microcontroller side)

Each relay is driven via an opto-isolated low-side driver (or a relay module
with built-in optocoupler). Active-low or active-high depends on the module;
verify with a multimeter before connecting mains.

For a typical "active-low" relay module:
- GPIO LOW  → relay energized → state = 1 (COM↔NO)
- GPIO HIGH → relay de-energized → state = 0 (COM↔NC)

Provide an inversion flag in the driver so the high-level state machine
always uses logical `1 = energized`.

## Test plan (do this with mains DISCONNECTED first)

1. Verify all six wiring paths with a multimeter in continuity mode.
2. With low-voltage test source (e.g., 12 V DC across L/N terminals,
   spirals temporarily replaced with light bulbs or resistors), step through
   the four canonical states and verify expected current.
3. Forbidden transitions: deliberately attempt `0.5 kW → 2 kW` direct
   (requires bypassing the state machine) — confirm software refuses.
4. Power-on test: verify controller drives OFF state on boot before any
   user command is accepted.

## Glossary

- **SPDT**: Single Pole Double Throw — relay with one common contact and two
  output positions (NO and NC).
- **NO / NC**: Normally Open / Normally Closed contacts of the relay.
- **Make-before-break / break-before-make**: contact transfer style. Standard
  mechanical SPDT relays are break-before-make (both contacts briefly open
  during transition).
- **Arc**: brief plasma discharge between separating contacts, conductive for
  microseconds to milliseconds. Treated as a temporary closed circuit during
  transition for safety analysis.
