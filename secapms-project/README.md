# SECAPMS — Smart Emergency Corridor & Ambulance Priority Management System

A working prototype of the full plan, split into three independently
deployable pieces so each can be pushed to its own host with zero config:

```
secapms/
├── frontend/     — static web dashboard (public portal + 5 role portals)
├── backend/      — MQTT ⇄ Supabase bridge service (Node.js)
├── firmware/     — ESP32 code for ambulance / junction / hospital devices
└── README.md     — you are here
```

Each folder has its own README with details. This file is the map + the
fastest path to actually pushing all three somewhere.

## What's real vs. what you still need to provision

| Piece | Status |
|---|---|
| `frontend` | Fully working static app. Ships with realistic in-memory mock data (schema-shaped) so you can demo every role end-to-end today. Swapping the mock `db` object for real Supabase calls is a small, mechanical change once the backend + schema below are live. |
| `backend` | Real, runnable Node.js service. Needs your actual Supabase project URL/key and MQTT broker credentials in `.env` to do anything (it will start and log clearly if they're missing). |
| `backend/schema.sql` | The actual SQL for the schema described in the plan — paste it into Supabase's SQL editor once, and every table/RLS policy/trigger exists for real. |
| `firmware` | Real, compilable ESP32 sketches. Needs your Wi-Fi + broker credentials filled into `secapms_config.h` before flashing. |

## Fastest path to a live pilot

1. **Database**: create a Supabase project (or Lovable Cloud), run `backend/schema.sql` in the SQL editor.
2. **Broker**: stand up Mosquitto locally for testing, or HiveMQ Cloud for anything beyond your LAN.
3. **Backend**: `cd backend && cp .env.example .env`, fill in the values, `npm install && npm start` locally to confirm it connects, then deploy (Render/Railway/Fly — steps in `backend/README.md`).
4. **Firmware**: flash one of each device (`firmware/README.md`), confirm they show up as `online` via `mosquitto_sub -t 'secapms/#' -v` and that events land in Supabase's table editor.
5. **Frontend**: deploy `frontend/` as-is to see the full UI today; swap its mock data layer for real Supabase queries whenever you're ready to go live end-to-end (the shapes already match — see comments at the top of `frontend/index.html`).

## Push this to GitHub

```bash
cd secapms
git init
git add .
git commit -m "SECAPMS v1: frontend, backend bridge, firmware"
git branch -M main
git remote add origin https://github.com/<you>/secapms.git
git push -u origin main
```

`.gitignore` files are already in place for `frontend/` and `backend/` so `node_modules/` and `.env` never get committed.

## Deploy each piece

### Frontend (pick one, both are zero-config from this repo)
- **Vercel**: import the GitHub repo, set **Root Directory** to `frontend`. `vercel.json` is already there — no build step needed, it's a static file.
- **Netlify**: import the repo, set **Base directory** to `frontend`. `netlify.toml` is already configured.

### Backend
- **Render / Railway / Fly.io / Docker** — full step-by-step for each in `backend/README.md`. All have free tiers suitable for a pilot.

### Firmware
- Flashed directly to ESP32 hardware over USB from the Arduino IDE — see `firmware/README.md`. After first flash, updates can go out over Wi-Fi via OTA.

## Design system (unchanged, locked by the original plan)
Navy Trust palette (`#0f1b3d` / `#1e3a5f` / `#3b6fa0` / `#e8edf3`, red/green/amber status colors), Sora for headings/KPIs, Manrope for body. Persistent sidebar, top status strip, dense multi-panel layout, government control-room aesthetic.
