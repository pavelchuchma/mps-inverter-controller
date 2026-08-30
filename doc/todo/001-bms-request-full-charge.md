---
id: 001
title: React to the BMS "request full charge" flag (0x35C bit 3)
type: enhancement
status: open
priority: low
component: battery-can
created: 2026-08-30
---

# React to the BMS "request full charge" flag (0x35C bit 3)

## Summary

The Pylontech BMS can ask to be fully charged. Per the vendor specification
(`CAN-Bus-protocol-PYLON-low-voltage-V1.2-20180408.pdf`, table 5, see
[`../battery_can_spec.md`](../battery_can_spec.md)):

> **Request full charge:** Reason: when battery is not full charged for long
> time, the accumulative error of SOC calculation will be too high and may not
> able to be charged or discharged as expected capacity.
> Logic: if SOC never higher than 97 % in 30 days, will set this flag to 1. And
> when the SOC is ≥ 97 %, the flag will be 0.
> How to: we suggest inverter to charge the battery by grid when this flag is 1.

This is the only signal in the whole system that points *against* dumping PV
surplus into the boiler. Every other input — CCL, charge current, PV voltage —
is about finding surplus to burn. This one says *stop burning it and let the
battery fill*, and nothing else can say so: the console link does not carry the
flag, and SoC alone cannot distinguish "96 % today" from "never above 97 % for a
month".

The vendor's own remedy is grid charging, which this installation does not have.
Withholding the boiler dump load is the only lever available here.

## Why not now

The flag should never be set in the current operating regime:

- The battery reaches 100 % SoC essentially **every day**, which clears the flag
  (the clear condition is SoC ≥ 97 %).
- The boiler actually runs about **1 h/day** — the dump load is nowhere near
  large enough or long enough to hold SoC below 97 % for 30 consecutive days.
- Overall cottage consumption is minimal.

So the 30-day condition is not reachable today. This is parked, not dropped.

**What would change that**, and should reopen it:

- A long overcast spell or a deep-winter stretch where SoC does not reach 97 %
  for weeks. This is the realistic trigger — it is seasonal, not hypothetical.
- Boiler regulation becoming more aggressive (e.g. once CCL feeds
  `autoRegulate()`), to the point where it routinely caps SoC below 97 %.
- Cottage consumption growing.

**Detection before action.** The flag is stored as `full_chg_req` in the
`chajda-battery-can` measurement from the moment the CAN storage work lands (see
[`../battery_can_data_spec.md`](../battery_can_data_spec.md), commit 2). That
turns "we assume it never sets" into something observed rather than believed. A
Grafana alert on `full_chg_req == true` is the cheap way to be told when this
assumption expires — worth adding with the other panels, well before any control
code.

## Expected behavior

While the BMS asserts the flag, the controller should stop competing with the
battery for PV surplus, and resume once the BMS clears it at SoC ≥ 97 %.

No extra hysteresis is needed on our side: the BMS sets at "30 days without
97 %" and clears at "≥ 97 %", which is already a wide, well-damped band.

## Proposed fix

In `autoRegulate()` (`relay.cpp`), gate the two step-up paths — charge surplus
and throttled surplus — on the flag being clear. Open question whether asserting
the flag should also force the boiler down a step or straight to OFF, or merely
freeze it where it is; blocking step-ups is the minimal version and probably
enough, since the discharge rule already backs the boiler off when the panels
cannot carry it.

Constraints carried over from the CAN design:

- **Fail-open.** If `!pylontech_can_valid()`, behave exactly as today. The flag
  is an additional restriction, never a requirement — a CAN dropout must not
  turn into a boiler outage.
- Log the transition through `printInfo()` and let `influx_log_event()` capture
  the surrounding state, so the day the flag first appears is legible afterwards.

Depends on the CAN parse + store work (commits 1 and 2 of the implementation
plan in [`../battery_can_data_spec.md`](../battery_can_data_spec.md)) — the flag
has to be decoded and stored before anything can react to it.

## Resolution

_Not resolved yet._
