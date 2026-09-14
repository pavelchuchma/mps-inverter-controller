# Grafana dashboard

`chajda.json` is the dashboard that visualises everything this firmware writes
to InfluxDB. It lives here rather than with the Grafana stack itself because the
panels are tied to the measurements in [`../src/influx.cpp`](../src/influx.cpp):
a field renamed there breaks a panel here, and the two should move together.

- **Dashboard:** <http://10.200.0.150:3000/d/chajda/chajda> (uid `chajda`)
- **Deployment:** the Grafana + InfluxDB docker stack lives in a separate repo,
  `~/work/HomeAutomation/grafana/` — `docker-compose.yml` (grafana-oss 13.x and
  influxdb 2.x on the Raspberry Pi), datasource provisioning, and `backup.sh`,
  which rsyncs both docker volumes to the external HDD.
- **Host:** `10.200.0.150` over the WireGuard tunnel, `pi.local` on the LAN.
  Grafana on `:3000`, InfluxDB on `:8086` (org `home`, bucket `metrics`).
- **Credentials** for both are in the comment block of `include/credentials.h`,
  which is git-ignored. Nothing here contains a token or password.

## Measurements the dashboard reads

| Measurement | Written by | Cadence |
| --- | --- | --- |
| `chajda-inverter` | QPIGS via `inverter_comm.cpp` | metrics grid (10 s) |
| `chajda-inverter-config` | QPIRI via `inverter_comm.cpp` | one point per read (5 min) |
| `chajda-battery` | `pwr` console via `pylontech_comm.cpp` | metrics grid |
| `chajda-battery-can` | CAN broadcast via `pylontech_can.cpp` | metrics grid + events |
| `chajda-can-link` | CAN link counters | once per flush (1 min) |
| `chajda-boiler` | relay state and water temperatures | metrics grid |
| `chajda-phone` | charger relay and phone status | metrics grid |

See [`../doc/battery_can_data_spec.md`](../doc/battery_can_data_spec.md) for the
storage tiers behind the CAN measurements, and
[`../doc/todo/_005-store-inverter-charge-config.md`](../doc/todo/_005-store-inverter-charge-config.md)
for the configuration series.

## Export and import

The dashboard is **not** provisioned from a file — Grafana serves it from its own
database, so this copy is a backup and a review artefact, not the running
version. Editing it here changes nothing until it is pushed.

Export what is running now, over the checked-in copy:

```sh
GFA=$(grep -m1 -i "HTTP Basic auth, admin:" include/credentials.h \
      | sed 's/.*admin:\([^ .]*\).*/admin:\1/')
curl -s -u "$GFA" http://10.200.0.150:3000/api/dashboards/uid/chajda \
  | python3 -c "import json,sys; f=open('grafana/chajda.json','w'); \
      json.dump(json.load(sys.stdin)['dashboard'], f, ensure_ascii=False, \
                indent=2, sort_keys=True); f.write('\n')"
```

Push this copy back (restore, or apply an edit made here):

```sh
python3 -c "import json; d=json.load(open('grafana/chajda.json')); \
  json.dump({'dashboard': d, 'overwrite': True}, open('/tmp/push.json','w'))"
curl -s -u "$GFA" -X POST -H "Content-Type: application/json" \
  -d @/tmp/push.json http://10.200.0.150:3000/api/dashboards/db
```

Keys are sorted and the indent fixed on export. Grafana does not keep key order
stable across saves, so without that every re-export would diff as the whole
file and the history would be useless.

## Older versions

There is no need to keep dated copies of this file: Grafana stores every save in
its own database, with the `message` each push carried.

```sh
curl -s -u "$GFA" \
  "http://10.200.0.150:3000/api/dashboards/uid/chajda/versions?limit=40"
curl -s -u "$GFA" \
  http://10.200.0.150:3000/api/dashboards/uid/chajda/versions/<version>
```

The same list is in the UI under dashboard settings → Versions, which also
diffs two versions and restores one. That history lives in the `grafana-data`
docker volume, which `backup.sh` in the stack repo rsyncs to the external HDD,
so it survives the Pi. This checked-in copy is the one that survives the *Pi
being rebuilt from scratch* — that is what it is for, not day-to-day undo.

`overwrite: true` above is for a deliberate restore. When applying an edit,
prefer keeping the `version` field from the export and sending `overwrite:
false`: the write is then refused if somebody changed the dashboard in the UI in
the meantime, instead of silently discarding their change.

## Conventions worth keeping

- **Series names carry their source**: `I:` inverter (QPIGS), `B:` battery
  console, `CAN:` BMS over CAN, `INV:` inverter configuration (QPIRI). The same
  quantity read over two links appears twice on purpose — the difference between
  them is the standing check that both are still decoding correctly.
- **A gap must look like a gap.** Every panel has `spanNulls: false` and every
  query `createEmpty: true`, so missing data breaks the line instead of being
  bridged by a straight segment across an outage.
- **Except the configuration series.** `chajda-inverter-config` is sampled every
  5 minutes, which on the ~25 s aggregation window of the default 6 h view would
  be null in eleven windows out of twelve and break at every one of them. Those
  series use `createEmpty: false`, `spanNulls: 900000` (15 min) and
  `stepAfter`, applied per series through field overrides because they share
  panels with 10 s measurements. A fixed threshold is safe only here, where the
  point spacing is pinned by the poll cadence rather than by the zoom level.
