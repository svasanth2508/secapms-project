import "dotenv/config";
import express from "express";
import { startMqttBridge } from "./mqttBridge.js";
import { startRealtimeBridge } from "./realtimeBridge.js";

console.log("=== SECAPMS backend bridge starting ===");

const mqttClient = startMqttBridge();
startRealtimeBridge(mqttClient);

// Minimal HTTP surface: just a health check so hosting platforms
// (Render/Railway/Fly.io) have something to poll and know the process
// is alive. This service otherwise does all its work over MQTT and
// Supabase Realtime, not HTTP.
const app = express();
app.get("/", (_req, res) => res.json({ ok: true, service: "secapms-backend" }));
app.get("/healthz", (_req, res) => res.json({ ok: true, mqtt: mqttClient.connected }));

const port = process.env.PORT || 8080;
app.listen(port, () => console.log(`[http] health check listening on :${port}`));
