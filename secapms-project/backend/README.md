# SECAPMS Backend — MQTT ⇄ Supabase Bridge

This is the missing piece from the plan: the service that takes messages
from real ESP32 hardware (over MQTT) and turns them into rows in your
Supabase database, and takes database changes (admin approves an
emergency) and turns them back into commands sent out to hardware.

It does **not** serve the dashboard — that's `../frontend`. This is a
small, always-on background worker plus a one-route health check.

## What it does

```
ESP32 devices  --MQTT-->  [this bridge]  --Supabase client-->  Postgres
Postgres  --Realtime subscription-->  [this bridge]  --MQTT-->  ESP32 devices
```

- `src/mqttBridge.js` — subscribes to `secapms/v1/#`, writes ambulance
  telemetry into `gps_logs`, new activations into `emergency_events`,
  junction heartbeats into `device_health`, and device online/offline
  state into `devices`.
- `src/realtimeBridge.js` — subscribes to Postgres changes on
  `emergency_events` and publishes `priority_green`/`normal` commands to
  junction controllers, status updates to the ambulance unit, and alerts
  to the hospital panel.
- `src/index.js` — wires both up and exposes `GET /healthz`.

## 1. Set up Supabase

1. Create a project in Lovable Cloud or directly in [Supabase](https://supabase.com).
2. Open **SQL Editor**, paste in `schema.sql` from this folder, run it. This creates every enum, table, RLS policy, and trigger from the plan.
3. Go to **Project Settings → API**, copy the **Project URL** and the **`service_role` key** (not the anon key — this backend needs to bypass RLS since devices don't have their own Supabase Auth sessions).

## 2. Set up your MQTT broker
Use the same broker your ESP32 devices are configured for in
`../firmware/common/secapms_config.h` — either a local Mosquitto instance
for bench testing, or a hosted TLS broker like HiveMQ Cloud for anything
beyond your own LAN. See `../firmware/README` for broker setup details.

## 3. Configure and run locally
```bash
cd backend
cp .env.example .env
# edit .env: SUPABASE_URL, SUPABASE_SERVICE_ROLE_KEY, MQTT_URL, MQTT_USERNAME, MQTT_PASSWORD
npm install
npm start
```
You should see `[mqtt] connected` and `[realtime] emergency_events channel: SUBSCRIBED` in the logs. Confirm end-to-end by publishing a fake activation:
```bash
mosquitto_pub -h <broker-host> -t 'secapms/v1/ambulance/AU-4521/event' \
  -m '{"device_id":"AU-4521","patient_category":"critical","lat":13.06,"lng":80.25}'
```
…then check the Supabase table editor for a new `pending` row in `emergency_events`.

## 4. Deploy it (pick one — all have free tiers)

### Render
1. Push this repo to GitHub.
2. New → Web Service → connect the repo, set **Root Directory** to `backend`.
3. Build command: `npm install`. Start command: `npm start`.
4. Add the environment variables from `.env.example` in the dashboard.
5. Deploy — Render gives you a URL; `GET /healthz` should return `{"ok":true}`.

### Railway
1. New Project → Deploy from GitHub repo, set the service root to `backend`.
2. Railway auto-detects Node; add the same environment variables.
3. Deploy.

### Fly.io
1. `fly launch` from inside `backend/` (it will detect Node and offer to write a `fly.toml` — accept, or use the included `Dockerfile`).
2. `fly secrets set SUPABASE_URL=... SUPABASE_SERVICE_ROLE_KEY=... MQTT_URL=... MQTT_USERNAME=... MQTT_PASSWORD=...`
3. `fly deploy`.

### Docker (any VPS)
```bash
docker build -t secapms-backend .
docker run -d --env-file .env -p 8080:8080 secapms-backend
```

## Notes and honest limitations
- The corridor-junction lookup in `realtimeBridge.js` currently opens/closes every junction tagged `corridor_group = 'C1'` as a simple starting point — it does **not** yet compute the actual shortest path between the ambulance's live position and the destination hospital. That routing logic is the natural next thing to build once you have real GPS data flowing.
- This bridge assumes one broker and one Supabase project. If you run multiple ambulance fleets/cities, give each its own `MQTT_TOPIC_PREFIX` and a separate bridge instance per prefix.
- Keep the service-role key out of the frontend and out of any device — it only ever belongs in this backend's environment variables.
