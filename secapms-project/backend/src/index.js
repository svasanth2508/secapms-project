import "dotenv/config";
import express from "express";
import cors from "cors";

import { startMqttBridge } from "./mqttBridge.js";
import { startRealtimeBridge } from "./realtimeBridge.js";
import { supabase } from "./supabaseClient.js";

console.log("=== SECAPMS backend bridge starting ===");

// ----------------------------------------------------
// Start MQTT + Realtime Bridge
// ----------------------------------------------------
const mqttClient = startMqttBridge();
startRealtimeBridge(mqttClient);

// ----------------------------------------------------
// Express App
// ----------------------------------------------------
const app = express();

app.use(cors());
app.use(express.json());

// ----------------------------------------------------
// Health Endpoints
// ----------------------------------------------------
app.get("/", (_req, res) => {
  res.json({
    ok: true,
    service: "secapms-backend",
    mqtt: mqttClient.connected,
  });
});

app.get("/healthz", (_req, res) => {
  res.json({
    ok: true,
    mqtt: mqttClient.connected,
  });
});

// ----------------------------------------------------
// Hospitals
// ----------------------------------------------------
app.get("/api/hospitals", async (_req, res) => {
  try {
    const { data, error } = await supabase
      .from("hospitals")
      .select("*");

    if (error) throw error;

    res.json(data);
  } catch (err) {
    console.error(err);
    res.status(500).json({
      error: err.message,
    });
  }
});

// ----------------------------------------------------
// Ambulances
// ----------------------------------------------------
app.get("/api/ambulances", async (_req, res) => {
  try {
    const { data, error } = await supabase
      .from("ambulances")
      .select("*");

    if (error) throw error;

    res.json(data);
  } catch (err) {
    console.error(err);
    res.status(500).json({
      error: err.message,
    });
  }
});

// ----------------------------------------------------
// Junctions
// ----------------------------------------------------
app.get("/api/junctions", async (_req, res) => {
  try {
    const { data, error } = await supabase
      .from("junctions")
      .select("*");

    if (error) throw error;

    res.json(data);
  } catch (err) {
    console.error(err);
    res.status(500).json({
      error: err.message,
    });
  }
});

// ----------------------------------------------------
// Devices
// ----------------------------------------------------
app.get("/api/devices", async (_req, res) => {
  try {
    const { data, error } = await supabase
      .from("devices")
      .select("*");

    if (error) throw error;

    res.json(data);
  } catch (err) {
    console.error(err);
    res.status(500).json({
      error: err.message,
    });
  }
});

// ----------------------------------------------------
// Emergency Events
// ----------------------------------------------------
app.get("/api/emergency-events", async (_req, res) => {
  try {
    const { data, error } = await supabase
      .from("emergency_events")
      .select("*")
      .order("created_at", { ascending: false });

    if (error) throw error;

    res.json(data);
  } catch (err) {
    console.error(err);
    res.status(500).json({
      error: err.message,
    });
  }
});

// ----------------------------------------------------
// Start Server
// ----------------------------------------------------
const port = process.env.PORT || 8080;

app.listen(port, () => {
  console.log(`HTTP Server running on port ${port}`);
});