import { supabase } from "./supabaseClient.js";

const PREFIX = process.env.MQTT_TOPIC_PREFIX || "secapms/v1";

/**
 * Listens for changes on emergency_events (admin approvals, hospital
 * readiness, completion) and pushes the resulting commands back out over
 * MQTT to the exact devices affected - ambulance status LEDs, junction
 * priority-green relays, and the hospital alert panel.
 */
export function startRealtimeBridge(mqttClient) {
  const channel = supabase
    .channel("emergency_events_bridge")
    .on(
      "postgres_changes",
      { event: "*", schema: "public", table: "emergency_events" },
      async (payload) => {
        try {
          await onEmergencyEventChange(mqttClient, payload);
        } catch (err) {
          console.error("[realtime] failed handling change:", err.message);
        }
      }
    )
    .subscribe((status) => {
      console.log("[realtime] emergency_events channel:", status);
    });

  return channel;
}

async function onEmergencyEventChange(mqttClient, payload) {
  const row = payload.new;
  if (!row) return;

  // 1. Tell the ambulance unit's LED/buzzer what state we're in.
  if (row.ambulance_id) {
    const device = await findDeviceBy("ambulance_id", row.ambulance_id);
    if (device) {
      publish(mqttClient, `${PREFIX}/ambulance/${device.external_id}/status`, {
        status: row.status,
        event_id: row.id,
      });
    }
  }

  // 2. On approval, open the corridor: tell every junction on this
  //    ambulance's corridor group to force priority green; on completion
  //    or cancellation, release them back to normal cycling.
  if (["approved", "in_progress"].includes(row.status)) {
    await broadcastToCorridorJunctions(mqttClient, "priority_green");
  } else if (["completed", "cancelled", "rejected"].includes(row.status)) {
    await broadcastToCorridorJunctions(mqttClient, "normal");
  }

  // 3. On approval, alert the destination hospital's desk panel.
  if (row.status === "approved" && row.hospital_id) {
    const device = await findDeviceBy("hospital_id", row.hospital_id);
    if (device) {
      publish(mqttClient, `${PREFIX}/hospital/${device.external_id}/alert`, {
        event_id: row.id,
        patient_category: row.patient_category,
      });
    }
  }
}

// NOTE: this reference implementation opens/closes every junction tagged
// with corridor_group "C1" as a simple starting point. A production
// version should resolve the *specific* corridor between the ambulance's
// current position and its destination hospital (e.g. shortest path over
// the junctions table) rather than a single hardcoded group.
async function broadcastToCorridorJunctions(mqttClient, command) {
  const { data: junctions } = await supabase
    .from("junctions")
    .select("id")
    .eq("corridor_group", "C1");
  if (!junctions) return;

  for (const j of junctions) {
    const device = await findDeviceBy("junction_id", j.id);
    if (device) {
      publish(mqttClient, `${PREFIX}/junction/${device.external_id}/command`, { command });
    }
  }
}

async function findDeviceBy(column, value) {
  const { data, error } = await supabase
    .from("devices")
    .select("id, external_id")
    .eq(column, value)
    .maybeSingle();
  if (error) console.error(`[device] lookup by ${column} failed:`, error.message);
  return data;
}

function publish(mqttClient, topic, obj) {
  mqttClient.publish(topic, JSON.stringify(obj), { qos: 1 }, (err) => {
    if (err) console.error(`[mqtt] publish failed on ${topic}:`, err.message);
    else console.log(`[mqtt] -> ${topic}`, obj);
  });
}
