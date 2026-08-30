# Battery CAN – Data Model and Storage (proposal)

Stage 2 of [`battery_can_spec.md`](battery_can_spec.md): turn the bring-up
sniffer into a parsed, published state and decide **what is kept, where, and how
often**. Stage 3 (feeding CCL into the boiler logic) is sketched at the end but
is deliberately not part of this proposal.

The link itself — wiring, transceiver, pin choice, frame layouts — is specified
in `battery_can_spec.md` and is not repeated here.

## Design constraints this has to respect

1. **Data volume is explicitly not a constraint yet.** Neither the InfluxDB
   footprint nor the transfer size is being optimised at this stage. Store what
   is useful; revisit once the software is tuned.
2. **The sample cadence is going to get slower, not faster.** The current 10 s
   grid (`METRICS_SAMPLE_INTERVAL_MS`) is a tuning-phase setting and will be
   relaxed once the boiler logic is dialled in. Nothing here may depend on 10 s
   specifically — see [Cadence independence](#cadence-independence).
3. **The console link stays.** Both links run in parallel and overlap on
   voltage / current / temperature / SoC. That overlap is an asset (see
   [Cross-check](#why-store-the-overlapping-fields-twice)), not redundancy to be
   removed.
4. **Nothing per-frame goes to flash.** `7d0ef34` removed exactly that pattern
   from the console link.
5. **The frame encoding is settled by the vendor specification.** All seven
   frames, every field and every alarm bit are transcribed in
   `battery_can_spec.md` from
   `CAN-Bus-protocol-PYLON-low-voltage-V1.2-20180408.pdf`. Nothing about the
   layout has to be inferred, and [`GET /can`](#diagnostics-get-can) only has to
   confirm that this pack matches the document. What still needs a full cycle is
   behavioural (does CCL taper), answered from tier B.
6. **The `metrics` bucket has unlimited retention** (`everySeconds: 0`,
   verified against the RPi; the read token is in the `credentials.h` comment
   block). Nothing written here expires by itself.

## Module shape

`pylontech_can.cpp` grows from a printer into a parser. Structure is
**burst-oriented**, not frame-oriented:

```
twai_receive(timeout 20 ms)
  └─ frame → decode into a task-local scratch struct, stamp it
  └─ no frame for CAN_BURST_GAP_MS (50 ms), or 200 ms since burst start
       └─ end of burst:
            1. range-check the scratch  (stage 1 only, see below)
            2. commit to the shared struct under the mutex   (one take / second)
            3. fold current_a into the min/max accumulator
            4. evaluate the event-log triggers
```

Committing per burst rather than per frame means a reader never sees `ccl_a`
from burst N next to `current_a` from burst N−1. The BMS sends the whole set
within ~10 ms, so the burst detector is unambiguous at the measured 2 s repeat
rate. It also gives the natural per-burst hook for the accumulators below.

### Published state

```c
struct PylontechCanState {
  // 0x351 — limits  (bytes 6-7: blank per the vendor spec, but this pack
  //                   sends 45.0 V there — not decoded, see battery_can_spec.md)
  float    charge_v;       // recommended charge voltage
  float    ccl_a;          // charge current limit — the reason for this link
  float    dcl_a;          // discharge current limit
  // 0x355
  int      soc;            // %
  int      soh;            // %

  // 0x356
  float    voltage_v;
  float    current_a;      // + charge / - discharge
  float    temp_c;         // average cell temperature
  // 0x359 — kept as raw words, see note below
  uint32_t protection;     // bytes 0-1 (tables 1, 2), zero-extended
  uint32_t alarm;          // bytes 2-3 (tables 3, 4), zero-extended
  uint8_t  modules;        // byte 4
  // 0x35C
  uint8_t  request_flags;  // raw d[0]; bit accessors below
  // freshness, per frame group (millis() of the burst that carried it)
  uint32_t limits_ts_ms, soc_ts_ms, measured_ts_ms, alarms_ts_ms, requests_ts_ms;
};

bool pylontech_can_get(PylontechCanState* out);  // mutex-guarded copy
bool pylontech_can_valid();                      // 0x351+0x355+0x356 all fresh
```

`request_flags` is decoded through `inline bool` accessors —
`can_charge_enabled()`, `can_discharge_enabled()`, `can_force_charge_1()`,
`can_force_charge_2()`, `can_full_charge_requested()`. The two force-charge bits
stay separate: bit 5 is for an inverter that lets the pack shut down, bit 4 for
one that does not, and the vendor recommends bit 4 — collapsing them would throw
away which regime this pack is in.

`protection`, `alarm` and `request_flags` are stored as **raw words with named
accessors on top**, even though the vendor spec now gives the full bit tables.
The reason is no longer uncertainty about the layout — it is that the tables are
sparse. Only seven bits are defined across the four `0x359` bytes and five in
`0x35C`; everything else is blank in a specification dated 2018, for a product
released later. Keeping the raw value costs nothing and captures whatever a
newer firmware puts in the undefined bits, which named booleans would silently
discard.

### Link health, separate from the data

```c
struct PylontechCanLink {
  uint32_t rx_frames;      // total received since boot
  uint32_t rx_missed;      // driver rx-queue overflow
  uint32_t bus_errors;
  uint32_t recoveries;     // bus-off recoveries performed
  uint32_t tx_failed;      // transmits that did not go out; zero while
                           // CAN_SEND_HEARTBEAT is 0, which is the point
  uint32_t rejected;       // bursts dropped by the range check (stage 1 only)
  uint8_t  state;          // twai_state_t
  uint32_t last_rx_ms;
};
```

Kept apart because it is wanted precisely when `pylontech_can_valid()` is false
and the data series has a gap.

### Staleness watchdog

`CAN_STALE_MS = 10000` — five missed bursts at this pack's **measured 2 s**
cadence. (The proposal said 5000 on the assumption of 1 Hz; bring-up measured
180 frames/min over six identifiers, i.e. a burst every 2 s, so 5 s would
tolerate a single dropped burst and flap on jitter.) Replaces the console
link's `PYLONTECH_FAIL_INVALIDATE_THRESHOLD` counter; CAN has no poll cycles to
count.

### Transmit: nothing

The controller puts **nothing** on the bus. This contradicts both the vendor
specification and step 1.2.5 below, and it is the single most important thing
bring-up established.

The vendor spec asks the inverter to reply with `0x305` every second. Nothing on
this bus acknowledges that frame, and an unacknowledged frame is retransmitted
by the CAN controller at bus speed — so one queued reply walks the transmit
error counter to bus-off in milliseconds, spraying error flags across the bus
throughout. Those error flags corrupt the pack's own frames and raise its error
counters. After some forty minutes of it the pack stopped broadcasting
entirely; with the controller transmitting nothing it resumed after ~90 s and
has broadcast continuously since.

The sniffer had this right by accident: it started `0x305` only after 10 s of
silence and stopped it on the first received frame, so in normal operation it
transmitted nothing. Step 1.2.5 called that conditional a relic of not knowing
the requirement and said to delete it. The conditional was load-bearing.

Two things make this safe to leave as it is:

- **The acknowledge bit is unaffected and is mandatory.** `TWAI_MODE_NORMAL`
  acknowledges every received frame; that is a hardware-level dominant bit, not
  a transmitted frame, and it is what keeps the pack out of error-passive. This
  is emphatically *not* listen-only mode.
- **The transmit path is kept, disabled, with the fix already applied.** It sits
  behind `CAN_SEND_HEARTBEAT 0` and already sets `TWAI_MSG_FLAG_SS`, so a
  future re-enable cannot reproduce the retry storm — a single-shot failure
  costs one error-counter increment instead of an unbounded retry.

### Range check — bring-up scaffold, not a permanent gate

CAN's CRC already rejects corrupted frames, so a range check is **not** noise
filtering. Its only job is to catch a wrong layout assumption (byte order,
scaling) before a mis-scaled `ccl_a` can reach the boiler logic. That is a
stage-1 concern, and it expires with stage 1.

During bring-up a burst that falls outside these ranges is dropped whole and
`rejected` is incremented:

| field | accepted range |
|---|---|
| `charge_v` | 40 … 60 V |
| `ccl_a`, `dcl_a` | 0 … 500 A |
| `soc`, `soh` | 0 … 100 % |
| `voltage_v` | 30 … 60 V |
| `current_a` | −500 … 500 A |
| `temp_c` | −30 … 80 °C |

**Then it comes out.** Once `/can` has confirmed the layout, the encoding is
fixed for the life of the pack, and a permanent hardcoded range can only do two
things, both bad:

- **Misfire silently.** A range set too tight drops every burst,
  `pylontech_can_valid()` stays false, and tier B writes an empty series that
  looks identical to a dead link.
- **Break on an upgrade.** Add a second US5000 and CCL/DCL legitimately scale
  with module count — a `0 … 500 A` ceiling then rejects *correct* data, exactly
  when the system is being extended and least wants a mystery.

So after stage 1 the only gate on the published state is the
[staleness watchdog](#staleness-watchdog). Frames arriving means the data is
good, because the CRC already said so.

### What validates the data permanently

The console link reads the same voltage, current, temperature and SoC over an
independent wire. That is a **live reference**, not a guess about what the
values ought to be — it validates against reality rather than against a
hardcoded opinion about reality, and it does so continuously, for free, from
data that is already being stored.

So the standing correctness check is `chajda-battery` against
`chajda-battery-can` in Grafana (see
[Why store the overlapping fields twice](#why-store-the-overlapping-fields-twice)),
where a threshold can be tuned or alerted on without touching firmware. Nothing
in the ESP32 needs to hold an opinion about plausible ranges.

## Diagnostics: `GET /can`

`pio remote device monitor` is a poor standing diagnostic: it has to be attached
before the interesting moment, it only shows what happens while it is attached,
and it needs a laptop with the project checked out. `/can` answers "is the link
healthy and what is the BMS saying right now" from any browser, instantly,
including from a phone.

This is also where the encoding gets confirmed, once, in stage 1: the raw bytes
and the decoded values sit on the same response, so either they agree with the
documented layout or they do not.

It returns the last **raw** payload per identifier alongside the decoded values,
so byte order and scaling stay verifiable at any time — not just during
bring-up, and specifically after a firmware change that touches the parser:

```json
{
  "valid": true,
  "frames": {
    "351": {"raw":"10 02 C8 00 E8 03 C2 01", "age_ms": 1355, "count": 95},
    "355": {"raw":"5D 00 64 00 09 00 0F 00", "age_ms": 1355, "count": 95},
    "356": {"raw":"72 13 F5 FF B6 00",       "age_ms": 1355, "count": 93},
    "359": {"raw":"00 00 00 00 01 50 4E",    "age_ms": 1356, "count": 95},
    "35C": {"raw":"C0 00",                   "age_ms": 1355, "count": 93},
    "35E": {"raw":"50 59 4C 4F 4E 02 03 00", "age_ms": 1355, "count": 93}
  },
  "decoded": {"charge_v":52.8,"ccl_a":20.0,"dcl_a":100.0,
              "soc":93,"soh":100,"voltage_v":49.78,"current_a":-1.1,"temp_c":18.2,
              "protection":0,"alarm":0,"modules":1,"request_flags":192},
  "link": {"state":1,"rx":564,"missed":0,"bus_err":7,"recoveries":0,
           "tx_failed":0,"rejected":0,"last_rx_age_ms":1355}
}
```

The example above is a real response, not a sketch. Two details it fixes in the
original proposal: `state` is 1 for a healthy link (`TWAI_STATE_RUNNING`), not 0
— 0 is `TWAI_STATE_STOPPED` — and every field in `decoded` is **`null`** until
the frame group carrying it has actually arrived. A zero there would be
indistinguishable from a real measurement of zero, which is precisely the
confusion a diagnostic endpoint must not create.

Cost: one route, ~600 B of JSON, only on demand.

With this in place the sniffer's throttled per-frame printing becomes redundant
and should be cut back to transitions only, matching the app.log policy below —
`/can` carries the detail on demand, and `pio remote device monitor` is still
there if a genuine frame-by-frame trace is ever needed again.

## Storage tiers

Four tiers, from "never stored" to "stored forever":

| tier | where | cadence | contents |
|---|---|---|---|
| A | RAM (`PylontechCanState`) | every burst, ~0.5 Hz | full decoded set |
| B | InfluxDB `chajda-battery-can` | the metrics sample grid | the measured set + accumulators |
| C | InfluxDB, same measurement | on significant change, rate-limited | full snapshot at the moment of the change |
| D | InfluxDB `chajda-can-link` | once per flush | link health, written unconditionally |
| — | app.log | on state transition only | link up/down, bus-off, alarm change |

Raw 1 Hz frames are **not** stored anywhere. Neither is `0x35E` (constant
`PYLON`) or the per-frame timestamps.

### Cadence independence

Tier B rides `METRICS_SAMPLE_INTERVAL_MS` — whatever it is set to. It is
currently 10 s and will be relaxed once the boiler logic is tuned, so no
threshold, interval or field here is expressed in terms of 10 s.

The important consequence is that **the two accumulator mechanisms get more
valuable as the grid coarsens, not less.** Point-sampling a 1 Hz signal at 10 s
throws away nine of ten samples; at 60 s it throws away fifty-nine of sixty. The
min/max accumulator (tier B) and the change-triggered events (tier C) are what
keep the information that the point sample drops, and they are the reason the
grid *can* be relaxed without losing the ability to explain what happened.

Two rules follow:

- Tier C triggers are **not** rate-limited relative to the grid; their rate
  limit is absolute (`CAN_EVENT_MIN_INTERVAL_MS`). A coarser grid must not make
  a CCL step less visible.
- Once the grid goes above ~30 s, add `ccl_min_a` to tier B for the same reason
  `current_min_a` exists — at that point a point sample of CCL is no longer
  representative of the window.

### Tier B — `chajda-battery-can`, on the metrics grid

Written from `append_sample()` in `influx.cpp`, on the existing grid and into
the existing batch — one more measurement in the same POST, no new request
path. Emitted only while `pylontech_can_valid()`, so an outage leaves a gap in
Grafana rather than a run of zeros, exactly as `chajda-battery` and
`chajda-inverter` already do.

| field | type | frame | why it is worth keeping |
|---|---|---|---|
| `ccl_a` | float | 0x351 | the leading limit signal — the point of the whole link |
| `dcl_a` | float | 0x351 | discharge headroom |
| `charge_v` | float | 0x351 | recommended charge voltage; moves on temperature and during balancing |
| `soc` | int | 0x355 | cross-check vs console |
| `soh` | int | 0x355 | **new** — pack ageing, only readable over months |
| `voltage_v` | float | 0x356 | cross-check |
| `current_a` | float | 0x356 | cross-check |
| `power_w` | int | derived | parity with `chajda-battery` |
| `temp_c` | float | 0x356 | cross-check (average cell temp vs console pack 1) |
| `current_min_a` | float | accumulator | see below |
| `current_max_a` | float | accumulator | see below |
| `protection`, `alarm` | int | 0x359 | raw words, seven bits defined |
| `modules` | int | 0x359 | should read 1; a change means something broke |
| `chg_en`, `dchg_en` | bool | 0x35C | BMS's charge / discharge permission |
| `force_chg_1`, `force_chg_2` | bool | 0x35C | **new** — kept separate, the two bits mean different things |
| `full_chg_req` | bool | 0x35C | **new** — the 30-day calibration request, a stage-3 control input |
| `req_raw` | int | 0x35C | raw byte 0, so undefined bits are not lost |

**The min/max accumulator.** The CAN task sees `current_a` at 1 Hz; the sampler
reads it once per grid interval and everything in between is discarded. Folding
min and max into two floats per burst, and resetting them when the sampler
reads, turns each point into a *range* — a boiler step (±10 A) or a short
discharge spike stays visible without storing 1 Hz.

This is the mechanism that decouples the stored resolution from the grid: it
costs two floats regardless of whether the grid is 10 s or 60 s, and its value
rises as the grid is relaxed. Recommended, and it should go in before the grid
is slowed down, not after.

The same trick applies to `ccl_a` — see the rule under
[Cadence independence](#cadence-independence).

### Tier C — event points on significant change

Reuses the existing `influx_log_event()`, which captures a full snapshot of
*every* measurement off-grid and forces a flush within one sample interval —
the same mechanism `relay.cpp` already uses for boiler decisions. That is the
point: a CCL step and the boiler step it causes land on the same timeline in
Grafana, at their real times, independently of how coarse the grid is.

Triggers, evaluated once per burst:

- `ccl_a` differs from the last logged value by ≥ `CAN_EVENT_CCL_DELTA_A`
  (proposed **5 A**), **or** crosses to/from 0 (0 A = "do not charge", always
  worth a point regardless of the delta)
- `protection` or `alarm` changed — including in bits the vendor spec leaves
  undefined
- any `0x35C` request flag changed
- `pylontech_can_valid()` transitioned

Rate limit `CAN_EVENT_MIN_INTERVAL_MS` = **30 s** between event snapshots so a
dithering CCL cannot flood the batch. Deliberately an absolute limit, not a
multiple of the grid — relaxing the grid must not make a CCL step harder to see.
Expected rate: single digits per day in normal operation, since the taper is
close to monotone, with a burst only during an alarm — which is exactly when the
resolution is wanted.

### Tier D — `chajda-can-link`, once per flush

Written **unconditionally**, once per flush cycle (currently 1/min):
`rx_frames` (counter), `rx_rate` (frames/s since the last write), `missed`,
`bus_err`, `recoveries`, `tx_failed`, `state` (int), `valid` (bool), and — for
as long as the stage-1 range check exists — `rejected`.

`tx_failed` stays in the series even though the controller currently transmits
nothing, so it reads zero by construction. That is deliberate: it is the
tripwire for `0x305` ever being re-enabled, and the counter that showed the
retry storm in the first place. See [Transmit](#transmit-nothing) below.

Unconditional because this series is what explains a gap in tier B — it is
wanted precisely when the data series has none. For the first months of a
brand-new link on two strapping pins at the end of a 15 m cable, this is the
series that will actually be looked at.

`rx_rate` is derived from the frame counter and the elapsed time since the last
write, so it stays correct if the flush interval changes.

### app.log

Transitions only: link up/down, bus-off recovery performed, any change in
`protection`/`alarm`, any `0x35C` flag change, and — while the stage-1 range
check is still in — a sustained `rejected` rate. Never per frame.

## Why store the overlapping fields twice

Volume is not being optimised at this stage (constraint 1), so the question is
only whether the duplicated fields *earn* their place, not whether they fit.

They earn it twice over, because they carry the job the
[range check gives up](#what-validates-the-data-permanently): they are the only
standing check that the CAN values are right. That alone settles it — everything
below is the second reason.

The console link runs RS232 over 15 m and is corrupt often enough that it needed
three-way consensus voting (`PYLONTECH_CONSENSUS_COUNT`, commit `e540f1e`) and
per-field range validation (`a8180c9`) to stop bad values reaching InfluxDB.
Neither is a proof of correctness — they reduce the error rate, they do not
bound it.

CAN carries the same four measurements behind a 15-bit CRC. Storing both makes
`chajda-battery.voltage_v − chajda-battery-can.voltage_v` a directly plottable
measure of how often the console link is still wrong after all that filtering.
That answer decides two open questions: whether the console link's consensus
machinery can be simplified, and — if the divergence turns out to be material —
whether the boiler logic should move onto the CAN values, which are also 5×
fresher. Both are worth a few duplicated floats.

Note that the audit runs **both ways**. The obvious reading is "how often is the
console still wrong". The other reading is "is CAN still decoding correctly",
and that is the one that replaces the range check — with the difference that it
compares against a real second measurement instead of a hardcoded window, so it
cannot misfire on a legitimate value and does not need updating when the pack is
extended.

That two-way use is why these fields stay even after the console-link question
is answered.

## `/status` and the web UI

New short keys in `makeStatusJson()`, in the existing style:

| key | value |
|---|---|
| `cav` | CAN data valid |
| `ccl`, `dcl` | charge / discharge current limit [A] |
| `chv` | recommended charge voltage [V] |
| `soh` | state of health [%] |
| `cbv`, `cbc` | CAN voltage / current, for the cross-check row |

`data/index.html` + `app.js`: one row — `BMS: 100 A chg / 200 A dchg · SoH 100 %`
— greyed out when `cav` is false. Optionally a discreet warning when
`|bc − cbc| > 2 A`, which flags a console-link corruption in real time rather
than in retrospect.

## Stage 3 (later, not part of this proposal)

Sketch only, so the data model above is checked against where it is going:

- `ccl_a == 0` means the pack refuses charge. `autoRegulate()` currently infers
  this indirectly from high PV voltage plus near-zero charge current (the
  "throttled surplus" path B, `relay.cpp`). CCL is the direct statement of the
  same fact and could replace part of the morning-gate heuristic with something
  deterministic.
- `ccl_a` tapering below what the panels can deliver is a *leading* indicator
  that surplus is about to appear — it allows stepping the boiler up
  proactively, instead of waiting out `BOILER_RAISE_INTERVAL_MS` (15 min) on
  path B.
- **Request full charge (`0x35C` bit 3) is the surprise from the vendor spec,
  and it points the opposite way to everything else here.** The BMS sets it when
  SoC has not exceeded 97 % for 30 days, and clears it at ≥ 97 %. The reason is
  SoC-estimator drift: without a periodic full charge the accumulated error
  grows until the pack, in the vendor's words, "may not be able to be charged or
  discharged as expected capacity". Every other signal in this document is about
  finding surplus to dump into the boiler. This one says *stop dumping and let
  the battery fill*, and it is the only signal that can say so — the console
  link does not carry it, and SoC alone cannot distinguish "96 % today" from
  "never above 97 % for a month".

  The obvious response is to suppress boiler step-ups while the flag is set, and
  it is cheap: the flag is rare by construction, so the cost is a few days of
  colder water per month at most. Worth doing properly, because the failure it
  prevents is silent and cumulative. The vendor's own advice is to charge from
  the grid — not available here, which makes withholding the dump load the only
  lever this system has.

- **Force charge I / II (`0x35C` bits 5, 4)** have model-specific SoC
  thresholds, and the spec quotes them for US2000B and US2000B-Plus — **not**
  for the US5000. So the levels at which this pack asserts them are unknown and
  must be observed before anything depends on them.

- **Fail-open, always.** If `!pylontech_can_valid()`, `autoRegulate()` must
  behave exactly as it does today. CCL enters as an additional permission, never
  as a required one — otherwise a CAN dropout turns into a boiler outage. Note
  that `CAN_STALE_MS` (5 s) is deliberately aggressive for the *data*; control
  needs its own, longer tolerance, or it will flap.

One caveat to settle with real data first: the assumption that CCL tapers
smoothly as the pack fills is not certain for this firmware. Some Pylontech
revisions hold CCL at its maximum and do the whole CV phase through CVL alone,
dropping CCL only at very high SoC, low temperature, or under protection. If
that is what this pack does, CCL is still a useful *protection* signal but a
poor *taper* signal, and stage 3 looks different. This is behavioural, not a
question about encoding: a cycle of tier-B data answers it, which is why storage
lands before any control decision is designed.

## Implementation plan

Three commits, each independently useful and independently revertable. Steps
inside a commit are ordered so the firmware builds and runs after each one.

Firmware goes up with `platformio remote run -t upload`. **Ask before every
upload.** No `data/` re-upload is needed until step 2.6 touches `data/`.

### Commit 1 — parse and publish, consume nothing

Goal: `/can` shows this pack's frames decoded against the vendor spec. Nothing
is stored, no control path changes, so this cannot affect the boiler.

**1.1 `include/pylontech_can.h`** — replace the sniffer header with:

- `struct PylontechCanState` and `struct PylontechCanLink` as specified under
  [Published state](#published-state)
- `void pylontech_can_init(int tx_pin, int rx_pin);` (unchanged signature)
- `bool pylontech_can_get(PylontechCanState* out);`
- `bool pylontech_can_valid();`
- `void pylontech_can_get_link(PylontechCanLink* out);`
- `inline bool` accessors over `request_flags`: `can_charge_enabled()`,
  `can_discharge_enabled()`, `can_force_charge_1()`, `can_force_charge_2()`,
  `can_full_charge_requested()`
- constants: `CAN_STALE_MS` 5000, `CAN_BURST_GAP_MS` 50, `CAN_BURST_MAX_MS` 200,
  `CAN_HEARTBEAT_INTERVAL_MS` 1000

Keep the constants in the header, matching `pylontech_comm.h`.

**1.2 `src/pylontech_can.cpp`** — restructure the existing task:

1. Add `static SemaphoreHandle_t g_can_mutex`, `g_can_state`, `g_can_link`,
   mirroring `g_pylon_mutex` / `g_pylontech_status` in `pylontech_comm.cpp:7-10`
   so both links read the same way.
2. Replace the `should_print` / `print_frame` throttle machinery with
   `decode_frame(const twai_message_t&, PylontechCanState* scratch)` — a switch
   on the identifier writing into a task-local scratch struct, stamping the
   matching `*_ts_ms`. Keep the existing `u16le` / `i16le` helpers.
3. Burst detection in the receive loop: drop the `twai_receive` timeout from
   200 ms to 20 ms, and commit the scratch when either `CAN_BURST_GAP_MS` has
   passed with no frame or `CAN_BURST_MAX_MS` since the burst started.
4. On commit: range-check (step 1.3), take the mutex once, copy scratch into
   `g_can_state`, release.
5. **Transmit nothing.** This step originally said to send `0x305`
   unconditionally at 1 Hz and to delete the sniffer's
   `CAN_SILENCE_BEFORE_HEARTBEAT_MS` / `heartbeat_started` conditional as a
   relic of not knowing the requirement. That was wrong, and the conditional
   was load-bearing — see [Transmit](#transmit-nothing). The transmit path is
   kept behind `CAN_SEND_HEARTBEAT 0` with `TWAI_MSG_FLAG_SS` already applied,
   so re-enabling it cannot reproduce the retry storm.
6. Keep `recover_if_bus_off()` and `print_bus_status()` unchanged — both still
   earn their place.
7. Cut Serial output to transitions only: link up/down, bus-off recovery,
   `protection`/`alarm` change, `0x35C` change. Delete the per-frame printing;
   `/can` replaces it.

**1.3 Range check** — the stage-1 scaffold from
[Range check](#range-check--bring-up-scaffold-not-a-permanent-gate). Guard it
with `#define CAN_BRINGUP_RANGE_CHECK 1` so removing it in commit 2 is deleting
a block, not unpicking logic.

**1.4 `src/esp_webserver.cpp`** — add `handleCan()` returning the JSON under
[Diagnostics](#diagnostics-get-can): raw payload per identifier, decoded values,
link counters. Register `server.on("/can", HTTP_GET, handleCan)` next to
`/status` (line 374). Add `#include "pylontech_can.h"`.

To serve the raw payloads, `PylontechCanState` needs the last raw 8 bytes and
DLC per identifier. Keep that in a separate `g_can_raw[6]` array in
`pylontech_can.cpp` behind `pylontech_can_get_raw()` rather than bloating the
state struct — it is diagnostic data, not state.

**1.5 `src/main.cpp`** — no change. `pylontech_can_init()` is already called at
the same place with the same signature; only the comment above it needs
updating.

**Verification — one `/can` fetch, no waiting:**

```
curl -s http://10.200.0.30/can | python3 -m json.tool
```

- [x] `0x35E` decodes to `PYLON` (plus `02 03 00`)
- [x] `0x359` byte 4 = 1, bytes 5–6 = `50 4E`
- [x] `0x351` charge voltage 52.8 V, CCL 20.0 A, DCL 100.0 A — but
      **bytes 6–7 = `C2 01` = 45.0 V**, not `00 00`
- [x] `0x355` bytes 4–7 = **`09 00 0F 00`**, not `00`
- [x] `0x356` voltage and current match `/status` `bv`/`bc` to 7 mV and 0.09 A
- [x] `0x35C` byte 0 = `0xC0`, as expected
- [x] `link.rejected` = 0, `link.tx_failed` = 0, `state` = 1 (`RUNNING`; the
      proposal's "state = 0" was wrong — 0 is `STOPPED`)

Both mismatches are recorded in
[`battery_can_spec.md`](battery_can_spec.md#measured-behaviour-on-this-pack).
Neither field is decoded or stored, so neither blocks commit 2; what did block
it, and is now fixed, was the transmit behaviour — see
[Transmit](#transmit-nothing).

### Commit 2 — store

**2.1 `include/influx.h` / `src/influx.cpp`** — add the `chajda-battery-can`
block to `append_sample()`, after the existing `chajda-battery` block
(`influx.cpp:113-135`), gated on `pylontech_can_valid()` exactly as that block
is gated on `g_pylontech_data_valid`. Fields per
[Tier B](#tier-b--chajda-battery-can-on-the-metrics-grid). Reuse
`appendFloat` / `appendInt` / `appendBool`.

**2.2 Min/max accumulator** — in `pylontech_can.cpp`, fold `current_a` into
`g_can_min_a` / `g_can_max_a` on every burst commit. Expose
`pylontech_can_take_current_range(float* lo, float* hi)` which returns the pair
and resets it, so the reset is atomic with the read and the sampler is the only
consumer. Land this *with* 2.1, not after — the point is to have it before the
grid is relaxed.

**2.3 `chajda-can-link`** — written once per flush in `influx_task()`, next to
the `flush()` call, unconditionally. Needs elapsed-time tracking for `rx_rate`.

**2.4 Tier C events** — in `pylontech_can.cpp`, after the burst commit, evaluate
the triggers from [Tier C](#tier-c--event-points-on-significant-change) and call
`influx_log_event()`. `influx.h` is already included by nothing in
`pylontech_can.cpp` — add it. Rate-limit with `CAN_EVENT_MIN_INTERVAL_MS`.

**2.5 Remove the range check** — delete the `CAN_BRINGUP_RANGE_CHECK` block and
`link.rejected`. `/can` has confirmed the layout and the console cross-check
takes over as the standing validation.

**2.6 `/status` + UI** — add the keys from
[`/status` and the web UI](#status-and-the-web-ui) to `makeStatusJson()`, and
one row to `data/index.html` + `data/app.js`. **This is the only step needing a
`data/` upload** (the three-file `curl -F` from `CLAUDE.md`).

**2.7 Grafana** — two panels on dashboard `chajda`: CCL against battery current
and SoC, and `chajda-battery.voltage_v − chajda-battery-can.voltage_v`. Via the
dashboard API, backing up the JSON first.

**Verification:** after one flush cycle,

```
RTOK=$(grep -m1 '^// read influx db token:' include/credentials.h | sed 's/.*token: *//')
curl -s -XPOST -H "Authorization: Token $RTOK" -H "Content-Type: application/vnd.flux" \
  --data 'from(bucket:"metrics") |> range(start:-10m)
          |> filter(fn:(r)=>r._measurement=="chajda-battery-can") |> last()' \
  "http://10.200.0.150:8086/api/v2/query?org=home"
```

- [ ] all tier-B fields present, none zero-by-accident
- [ ] `chajda-can-link` present and `valid=true`
- [ ] console-vs-CAN voltage difference within a few tens of mV

Then **let it run a full charge/discharge cycle** and answer the question that
needs a history: does CCL taper as the pack fills, or does this firmware hold it
at maximum and do the CV phase through the charge voltage alone?

Only after that is `METRICS_SAMPLE_INTERVAL_MS` relaxed — the accumulators and
event triggers must already be in place.

### Commit 3 — control

Separate proposal, written once commit 2 has produced a cycle of data. Sketched
under [Stage 3](#stage-3-later-not-part-of-this-proposal). Not started before
the taper question is answered, because the answer decides what it looks like.

### Risk notes

- **Nothing in commits 1 and 2 touches `relay.cpp`.** The boiler cannot change
  behaviour, which is why the CAN work can proceed without a supervised window.
- **`0x305` now transmits unconditionally.** The one new failure mode: if the
  transceiver or wiring has a transmit-side fault that the receive-only sniffer
  never exercised, the controller will go bus-off and `recover_if_bus_off()`
  will loop. `link.tx_failed` and `link.recoveries` in `/can` make it visible
  within seconds of the first upload — check them on the first fetch.
- **Stack.** The CAN task keeps its 4096 bytes. Removing the per-frame
  `Serial.printf` calls frees more than the accumulators and JSON building add.

## References

- [`battery_can_spec.md`](battery_can_spec.md) — the link: hardware, pins, frame layouts
- [`pylontech_comm_spec.md`](pylontech_comm_spec.md) — the parallel console link
- `include/config.h` — `METRICS_SAMPLE_INTERVAL_MS`, `METRICS_SAMPLES_PER_FLUSH`
- `src/influx.cpp` — `append_sample()`, `influx_log_event()`
- `src/relay.cpp` — `autoRegulate()`, the stage-3 consumer
