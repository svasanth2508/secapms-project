import mqtt from "mqtt";

const client = mqtt.connect({
  host: "a9191f90.ala.asia-southeast1.emqxsl.com",
  port: 8883,
  protocol: "mqtts",
  username: "secapms",
  password: "vasanths",
  protocolVersion: 5,
  reconnectPeriod: 0,
});
console.log("MQTT options:", {
  host: "a9191f90.ala.asia-southeast1.emqxsl.com",
  username: process.env.MQTT_USERNAME,
  passwordLength: process.env.MQTT_PASSWORD?.length,
});

client.on("connect", () => {
  console.log("✅ CONNECTED TO EMQX");
  process.exit(0);
});

client.on("error", (err) => {
  console.error("❌ FAILED");
  console.error(err);
  process.exit(1);
});