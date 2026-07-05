import mqtt from "mqtt";
import { supabase } from "./supabaseClient.js";

const PREFIX = process.env.MQTT_TOPIC_PREFIX || "secapms/v1";

/**
 * Connects to EMQX Cloud and routes inbound MQTT messages
 * to the appropriate Supabase handlers.
 */
export function startMqttBridge() {
  console.log("===== MQTT DEBUG =====");
  console.log("URL:", "[" + process.env.MQTT_URL + "]");
  console.log("USERNAME:", "[" + process.env.MQTT_USERNAME + "]");
  console.log("PASSWORD LENGTH:", process.env.MQTT_PASSWORD?.length);
  console.log("======================");

const client = mqtt.connect({
  host: "a9191f90.ala.asia-southeast1.emqxsl.com",
  port: 8883,
  protocol: "mqtts",
  username: process.env.MQTT_USERNAME,
  password: process.env.MQTT_PASSWORD,
  clientId: "secapms-backend-" + Math.random().toString(16).slice(2),
  clean: true,
  reconnectPeriod: 5000,
  connectTimeout: 30000,
  protocolVersion: 5,
});

  client.on("connect", () => {
    console.log("[MQTT] Connected");

    client.subscribe(`${PREFIX}/#`, (err) => {
      if (err) {
        console.error("[MQTT] Subscribe failed:", err);
      } else {
        console.log("[MQTT] Subscribed to", `${PREFIX}/#`);
      }
    });
  });

  client.on("message", async (topic, payload) => {
    try {
      await handleMessage(topic, payload);
    } catch (err) {
      console.error("[MQTT MESSAGE ERROR]", err);
    }
  });

  client.on("error", (err) => {
    console.error("[MQTT ERROR]", err);
  });

  client.on("close", () => {
    console.log("[MQTT] Connection Closed");
  });

  client.on("reconnect", () => {
    console.log("[MQTT] Reconnecting...");
  });

  return client;
}

async function handleMessage(topic, payloadBuf) {
  const parts = topic.split("/");

  // Expected:
  // secapms/v1/ambulance/<id>/telemetry
  if (parts.length < 5) {
    console.warn("[MQTT] Invalid topic:", topic);
    return;
  }

  const [, , kind, externalId, subtopic] = parts;

  // Last Will Topic
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
  } else {
    console.log("[MQTT] Unhandled topic:", topic);
  }
}

// ------------------------------------------------------------------
// KEEP EVERYTHING BELOW THIS LINE EXACTLY AS YOU ALREADY HAVE IT
// ------------------------------------------------------------------

// async function handleAmbulanceTelemetry(...) { ... }
// async function handleAmbulanceActivation(...) { ... }
// async function handleJunctionHealth(...) { ... }
// async function handleHospitalAck(...) { ... }
// async function findDevice(...) { ... }