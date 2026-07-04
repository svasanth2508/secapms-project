import mqtt from "mqtt";
import { supabase } from "./supabaseClient.js";

const PREFIX = process.env.MQTT_TOPIC_PREFIX || "secapms/v1";

/**
 * Connects to HiveMQ Cloud and routes inbound MQTT messages
 * to the appropriate Supabase handlers.
 */
export function startMqttBridge() {
  console.log("======================================");
  console.log("Starting MQTT Bridge...");
  console.log("MQTT URL:", process.env.MQTT_URL);
  console.log("MQTT USER:", process.env.MQTT_USERNAME);
  console.log("MQTT PASS LENGTH:", process.env.MQTT_PASSWORD?.length);
  console.log("======================================");

  const client = mqtt.connect({
    host: "2e3c2cfd53374ed98b4555ed3053dea73.s1.eu.hivemq.cloud",
    port: 8883,
    protocol: "mqtts",

    username: process.env.MQTT_USERNAME,
    password: process.env.MQTT_PASSWORD,

    clientId:
      "secapms-backend-" + Math.random().toString(16).substring(2, 10),

    reconnectPeriod: 5000,
    connectTimeout: 30000,
    clean: true,
    rejectUnauthorized: true,
  });

  client.on("connect", () => {
    console.log("======================================");
    console.log("[MQTT] CONNECTED SUCCESSFULLY");
    console.log("======================================");

    client.subscribe(`${PREFIX}/#`, { qos: 1 }, (err) => {
      if (err) {
        console.error("[MQTT] Subscribe Error:", err);
      } else {
        console.log("[MQTT] Subscribed to:", `${PREFIX}/#`);
      }
    });
  });

  client.on("message", (topic, payload) => {
    console.log("[MQTT] Message Received:", topic);

    handleMessage(topic, payload).catch((err) => {
      console.error("[MQTT] Message Handler Error:", err);
    });
  });

  client.on("reconnect", () => {
    console.log("[MQTT] Reconnecting...");
  });

  client.on("offline", () => {
    console.log("[MQTT] Client Offline");
  });

  client.on("close", () => {
    console.log("[MQTT] Connection Closed");
  });

  client.on("end", () => {
    console.log("[MQTT] Connection Ended");
  });

  client.on("error", (err) => {
    console.error("======================================");
    console.error("[MQTT ERROR]");
    console.error(err);
    console.error("======================================");
  });

  return client;
}

async function handleMessage(topic, payloadBuf) {
  const parts = topic.split("/");
  const [, , kind, externalId, subtopic] = parts;

  if (subtopic === "lwt") {
    const online = payloadBuf.toString() === "online";

    await supabase
      .from("devices")
      .update({
        last_seen: new Date().toISOString(),
        online,
      })
      .eq("external_id", externalId);

    console.log(
      `[DEVICE] ${externalId} -> ${online ? "ONLINE" : "OFFLINE"}`
    );

    return;
  }

  let payload;

  try {
    payload = JSON.parse(payloadBuf.toString());
  } catch {
    console.warn(`[MQTT] Invalid JSON on ${topic}`);
    return;
  }

  if (kind === "ambulance" && subtopic === "telemetry") {
    await handleAmbulanceTelemetry(externalId, payload);
  } else if (kind === "ambulance" && subtopic === "event") {
    await handleAmbulanceActivation(externalId, payload);
  } else if (kind === "junction" && subtopic === "health") {
    await handleJunctionHealth(externalId, payload);
  } else if (kind === "hospital" && subtopic === "ack") {
    await handleHospitalAck(externalId, payload);
  }
}

async function handleAmbulanceTelemetry(externalId, payload) {
  const device = await findDevice(externalId);
  if (!device || !device.ambulance_id) return;

  const { data: activeEvent } = await supabase
    .from("emergency_events")
    .select("id")
    .eq("ambulance_id", device.ambulance_id)
    .in("status", ["approved", "in_progress"])
    .order("activated_at", { ascending: false })
    .limit(1)
    .maybeSingle();

  if (activeEvent) {
    await supabase.from("gps_logs").insert({
      event_id: activeEvent.id,
      ts: new Date().toISOString(),
      lat: payload.lat,
      lng: payload.lng,
      speed: payload.speed_kmph,
    });
  }

  await supabase
    .from("devices")
    .update({
      last_seen: new Date().toISOString(),
      online: true,
    })
    .eq("id", device.id);
}

async function handleAmbulanceActivation(externalId, payload) {
  const device = await findDevice(externalId);
  if (!device || !device.ambulance_id) return;

  const { data: ambulance } = await supabase
    .from("ambulances")
    .select("id,hospital_id")
    .eq("id", device.ambulance_id)
    .maybeSingle();

  const { data: driver } = await supabase
    .from("drivers")
    .select("id")
    .eq("ambulance_id", device.ambulance_id)
    .maybeSingle();

  const { error } = await supabase.from("emergency_events").insert({
    driver_id: driver?.id ?? null,
    ambulance_id: device.ambulance_id,
    hospital_id: ambulance?.hospital_id ?? null,
    patient_category: payload.patient_category || "critical",
    status: "pending",
    activated_at: new Date().toISOString(),
  });

  if (error)
    console.error("[EVENT] Insert Failed:", error.message);
  else
    console.log(
      `[EVENT] Emergency Created for ${externalId}`
    );
}

async function handleJunctionHealth(externalId, payload) {
  const device = await findDevice(externalId);
  if (!device) return;

  await supabase.from("device_health").insert({
    device_id: device.id,
    ts: new Date().toISOString(),
    uptime: payload.uptime_s,
    rssi: payload.rssi,
    online: true,
  });

  await supabase
    .from("devices")
    .update({
      last_seen: new Date().toISOString(),
      online: true,
    })
    .eq("id", device.id);
}

async function handleHospitalAck(externalId, payload) {
  if (!payload.event_id) return;

  await supabase.from("notifications").insert({
    kind: "hospital_ready_ack",
    payload: {
      event_id: payload.event_id,
      device: externalId,
    },
  });

  console.log(
    `[HOSPITAL] ${externalId} acknowledged event ${payload.event_id}`
  );
}

async function findDevice(externalId) {
  const { data, error } = await supabase
    .from("devices")
    .select("id,kind,ambulance_id,junction_id,hospital_id")
    .eq("external_id", externalId)
    .maybeSingle();

  if (error)
    console.error("[DEVICE] Lookup Failed:", error.message);

  return data;
}