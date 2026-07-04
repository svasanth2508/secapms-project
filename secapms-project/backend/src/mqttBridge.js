import mqtt from "mqtt";
import { supabase } from "./supabaseClient.js";

const PREFIX = process.env.MQTT_TOPIC_PREFIX || "secapms/v1";

/**
 * Connects to the broker and routes every inbound device message to the
 * right Supabase write, matching the topic layout documented in
 * firmware/common/secapms_config.h:
 *
 *   secapms/v1/ambulance/<id>/telemetry   -> gps_logs, ambulances (live position)
 *   secapms/v1/ambulance/<id>/event       -> emergency_events (new pending row)
 *   secapms/v1/junction/<id>/health       -> device_health, devices.last_seen
 *   secapms/v1/hospital/<id>/ack          -> emergency_events (hospital ready ack)
 *   secapms/v1/<kind>/<id>/lwt            -> devices.last_seen / online flag
 */
export function startMqttBridge() {
 console.log("MQTT CONFIG");
console.log("URL:", process.env.MQTT_URL);
console.log("USERNAME:", process.env.MQTT_USERNAME);
console.log("PASSWORD LENGTH:", process.env.MQTT_PASSWORD?.length);
  const client = mqtt.connect(process.env.MQTT_URL, {
    username: process.env.MQTT_USERNAME,
    password: process.env.MQTT_PASSWORD,
    clientId: "secapms-backend-" + Math.random().toString(16).slice(2),
    reconnectPeriod: 4000,
  });

  client.on("connect", () => {
    console.log("[mqtt] connected, subscribing to " + PREFIX + "/#");
    client.subscribe(`${PREFIX}/#`, { qos: 1 });
  });

  client.on("reconnect", () => console.log("[mqtt] reconnecting..."));
  client.on("error", (err) => console.error("[mqtt] error:", err.message));

  client.on("message", async (topic, payloadBuf) => {
    try {
      await handleMessage(topic, payloadBuf);
    } catch (err) {
      console.error(`[mqtt] failed handling ${topic}:`, err.message);
    }
  });

  return client;
}

async function handleMessage(topic, payloadBuf) {
  const parts = topic.split("/"); // ["secapms","v1","ambulance","<id>","telemetry"]
  const [, , kind, externalId, subtopic] = parts;

  // Retained last-will messages arrive as plain "online" / "offline" text,
  // everything else is JSON.
  if (subtopic === "lwt") {
    const online = payloadBuf.toString() === "online";
    await supabase
      .from("devices")
      .update({ last_seen: new Date().toISOString(), online })
      .eq("external_id", externalId);
    console.log(`[device] ${externalId} -> ${online ? "online" : "offline"}`);
    return;
  }

  let payload;
  try {
    payload = JSON.parse(payloadBuf.toString());
  } catch {
    console.warn(`[mqtt] non-JSON payload on ${topic}, ignoring`);
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

  // Find the currently active event for this ambulance, if any, so the
  // GPS point can be attached to it.
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
    .update({ last_seen: new Date().toISOString(), online: true })
    .eq("id", device.id);
}

async function handleAmbulanceActivation(externalId, payload) {
  const device = await findDevice(externalId);
  if (!device || !device.ambulance_id) return;

  const { data: ambulance } = await supabase
    .from("ambulances")
    .select("id, hospital_id")
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

  if (error) console.error("[event] insert failed:", error.message);
  else console.log(`[event] pending emergency created for ambulance ${externalId}`);
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
    .update({ last_seen: new Date().toISOString(), online: true })
    .eq("id", device.id);
}

async function handleHospitalAck(externalId, payload) {
  if (!payload.event_id) return;
  await supabase
    .from("notifications")
    .insert({
      kind: "hospital_ready_ack",
      payload: { event_id: payload.event_id, device: externalId },
    });
  console.log(`[hospital] ${externalId} acknowledged bed ready for event ${payload.event_id}`);
}

async function findDevice(externalId) {
  const { data, error } = await supabase
    .from("devices")
    .select("id, kind, ambulance_id, junction_id, hospital_id")
    .eq("external_id", externalId)
    .maybeSingle();
  if (error) console.error("[device] lookup failed:", error.message);
  return data;
}
