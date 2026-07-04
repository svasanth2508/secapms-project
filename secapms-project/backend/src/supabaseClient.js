import { createClient } from "@supabase/supabase-js";

const url = process.env.SUPABASE_URL;
const key = process.env.SUPABASE_SERVICE_ROLE_KEY;

if (!url || !key) {
  console.warn(
    "[supabase] SUPABASE_URL / SUPABASE_SERVICE_ROLE_KEY are not set. " +
    "The bridge will start but every database call will fail until you " +
    "fill in backend/.env (see .env.example)."
  );
}

// Service-role client: runs entirely server-side, bypasses RLS by design,
// because devices authenticate to MQTT (not to Supabase Auth) and this
// bridge is the trusted intermediary writing on their behalf.
export const supabase = createClient(url || "http://localhost", key || "placeholder", {
  auth: { persistSession: false },
});
