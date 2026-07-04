-- =====================================================================
-- SECAPMS — Supabase schema
-- Run this in Supabase Studio -> SQL Editor (or `supabase db push` if
-- you keep it as a migration) on a fresh Lovable Cloud / Supabase project.
-- Matches the schema described in the SECAPMS v1 plan exactly.
-- =====================================================================

-- ---------- Enums ----------
create type app_role as enum ('admin','driver','hospital','traffic','maintenance');
create type emergency_status as enum ('pending','approved','in_progress','completed','cancelled','rejected');
create type patient_category as enum ('critical','serious','stable');
create type device_kind as enum ('ambulance_unit','junction_controller','hospital_alert','ai_camera');

-- ---------- Tables ----------
create table profiles (
  id uuid primary key references auth.users(id) on delete cascade,
  full_name text,
  phone text,
  photo_url text,
  -- Only meaningful for hospital-role users: which hospital's desk they
  -- staff. The original plan didn't specify this mapping explicitly, so
  -- it's added here so the "hospital reads events assigned to their
  -- hospital" RLS rule below has something real to check against. Admin
  -- sets this when inviting a hospital-role user (Users & Roles page).
  hospital_id uuid,
  created_at timestamptz not null default now()
);

-- Kept separate from profiles for security: role checks go through a
-- security-definer function, never a client-readable join.
create table user_roles (
  user_id uuid not null references auth.users(id) on delete cascade,
  role app_role not null,
  primary key (user_id, role)
);

create table hospitals (
  id uuid primary key default gen_random_uuid(),
  name text not null,
  address text,
  lat double precision,
  lng double precision,
  phone text,
  beds_available int not null default 0,
  is_ready boolean not null default false
);

alter table profiles add constraint profiles_hospital_id_fkey foreign key (hospital_id) references hospitals(id);

create table ambulances (
  id uuid primary key default gen_random_uuid(),
  reg_number text not null unique,
  hospital_id uuid references hospitals(id),
  model text,
  status text not null default 'available'
);

create table drivers (
  id uuid primary key default gen_random_uuid(),
  profile_id uuid references profiles(id),
  license_number text,
  ambulance_id uuid references ambulances(id),
  on_duty boolean not null default false
);

create table junctions (
  id uuid primary key default gen_random_uuid(),
  name text not null,
  lat double precision,
  lng double precision,
  corridor_group text
);

create table devices (
  id uuid primary key default gen_random_uuid(),
  kind device_kind not null,
  external_id text not null unique,
  junction_id uuid references junctions(id),
  ambulance_id uuid references ambulances(id),
  hospital_id uuid references hospitals(id),
  firmware text,
  last_seen timestamptz,
  online boolean not null default false
);

create table device_health (
  device_id uuid references devices(id) on delete cascade,
  ts timestamptz not null default now(),
  battery numeric,
  rssi int,
  uptime bigint,
  online boolean,
  primary key (device_id, ts)
);

create table emergency_events (
  id uuid primary key default gen_random_uuid(),
  driver_id uuid references drivers(id),
  ambulance_id uuid references ambulances(id),
  hospital_id uuid references hospitals(id),
  patient_category patient_category not null default 'stable',
  status emergency_status not null default 'pending',
  activated_at timestamptz not null default now(),
  approved_at timestamptz,
  completed_at timestamptz,
  approved_by uuid references auth.users(id),
  notes text
);

create table gps_logs (
  event_id uuid references emergency_events(id) on delete cascade,
  ts timestamptz not null default now(),
  lat double precision,
  lng double precision,
  speed numeric,
  primary key (event_id, ts)
);

create table notifications (
  id uuid primary key default gen_random_uuid(),
  user_id uuid references auth.users(id),
  kind text not null,
  payload jsonb,
  read_at timestamptz
);

create table audit_logs (
  id uuid primary key default gen_random_uuid(),
  actor_id uuid references auth.users(id),
  action text not null,
  entity text not null,
  entity_id uuid,
  meta jsonb,
  ts timestamptz not null default now()
);

-- ---------- has_role() security-definer helper ----------
create or replace function has_role(check_role app_role)
returns boolean
language sql
security definer
stable
as $$
  select exists (
    select 1 from user_roles
    where user_id = auth.uid() and role = check_role
  );
$$;

-- ---------- New-user trigger: create profile + default role ----------
create or replace function handle_new_user()
returns trigger
language plpgsql
security definer
as $$
begin
  insert into profiles (id, full_name) values (new.id, new.raw_user_meta_data ->> 'full_name');
  -- Default role is 'driver'; change this default or remove the insert
  -- entirely if you'd rather admins assign every role by hand.
  insert into user_roles (user_id, role) values (new.id, 'driver');
  return new;
end;
$$;

create trigger on_auth_user_created
  after insert on auth.users
  for each row execute function handle_new_user();

-- ---------- Row Level Security ----------
alter table profiles enable row level security;
alter table user_roles enable row level security;
alter table hospitals enable row level security;
alter table ambulances enable row level security;
alter table drivers enable row level security;
alter table junctions enable row level security;
alter table devices enable row level security;
alter table device_health enable row level security;
alter table emergency_events enable row level security;
alter table gps_logs enable row level security;
alter table notifications enable row level security;
alter table audit_logs enable row level security;

-- profiles: user reads/updates own; admin reads all
create policy "profiles_select_own_or_admin" on profiles for select
  using (id = auth.uid() or has_role('admin'));
create policy "profiles_update_own" on profiles for update
  using (id = auth.uid());

-- user_roles: only admin writes; users read own row
create policy "user_roles_select_own_or_admin" on user_roles for select
  using (user_id = auth.uid() or has_role('admin'));
create policy "user_roles_admin_write" on user_roles for all
  using (has_role('admin')) with check (has_role('admin'));

-- reference tables: authenticated read, admin write
create policy "hospitals_read_authenticated" on hospitals for select using (auth.role() = 'authenticated');
create policy "hospitals_admin_write" on hospitals for all using (has_role('admin')) with check (has_role('admin'));

create policy "ambulances_read_authenticated" on ambulances for select using (auth.role() = 'authenticated');
create policy "ambulances_admin_write" on ambulances for all using (has_role('admin')) with check (has_role('admin'));

create policy "junctions_read_authenticated" on junctions for select using (auth.role() = 'authenticated');
create policy "junctions_admin_write" on junctions for all using (has_role('admin')) with check (has_role('admin'));

create policy "drivers_read_authenticated" on drivers for select using (auth.role() = 'authenticated');
create policy "drivers_admin_write" on drivers for all using (has_role('admin')) with check (has_role('admin'));

-- emergency_events: driver reads own; hospital reads events for their
-- hospital; traffic reads active; admin all
create policy "events_driver_own" on emergency_events for select
  using (driver_id in (select id from drivers where profile_id = auth.uid()));
create policy "events_hospital_assigned" on emergency_events for select
  using (has_role('hospital') and hospital_id = (select hospital_id from profiles where id = auth.uid()));
create policy "events_traffic_active" on emergency_events for select
  using (has_role('traffic') and status in ('approved','in_progress'));
create policy "events_admin_all" on emergency_events for all
  using (has_role('admin')) with check (has_role('admin'));
create policy "events_driver_insert_own" on emergency_events for insert
  with check (driver_id in (select id from drivers where profile_id = auth.uid()));

-- device_health / gps_logs: admin + role-scoped read
create policy "device_health_admin_read" on device_health for select using (has_role('admin') or has_role('maintenance'));
create policy "gps_logs_admin_read" on gps_logs for select using (has_role('admin') or has_role('traffic'));

-- audit_logs: admin only
create policy "audit_admin_only" on audit_logs for select using (has_role('admin'));

-- notifications: user reads own
create policy "notifications_own" on notifications for select using (user_id = auth.uid());

-- ---------- Public (anon) read of a narrow, non-PII slice ----------
-- Active emergency events, no driver/hospital identity exposed:
create view public_active_corridors as
  select id, status, patient_category, activated_at
  from emergency_events
  where status in ('approved','in_progress');
grant select on public_active_corridors to anon;

-- Junction locations are not sensitive:
create policy "junctions_public_read" on junctions for select using (auth.role() = 'anon');
grant select on junctions to anon;

-- ---------- Explicit GRANTs (RLS still applies on top of these) ----------
grant select, insert, update on profiles, user_roles, hospitals, ambulances, drivers,
  junctions, devices, device_health, emergency_events, gps_logs, notifications, audit_logs
  to authenticated;
grant select on hospitals, junctions to anon;

-- =====================================================================
-- Seed reference data (optional) — safe to skip; the frontend's Admin ->
-- "Seed reference data" button does the same thing from the UI once the
-- real Supabase calls are wired in per backend/README.md.
-- =====================================================================
-- insert into hospitals (name, lat, lng, beds_available, is_ready) values
--   ('St. Aloysius General', 13.0060, 80.2210, 6, true),
--   ('Marina City Hospital', 13.0480, 80.2820, 2, true);
