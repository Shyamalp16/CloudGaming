create extension if not exists pgcrypto;

create table if not exists public.reflex_games (
  id text primary key check (id ~ '^(steam:[0-9]{1,10}|manual:[a-z0-9][a-z0-9-]{0,63})$'),
  source text not null check (source in ('steam', 'manual')),
  steam_app_id bigint unique,
  title text not null check (length(title) between 1 and 160),
  artwork_url text,
  enabled boolean not null default true,
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now()
);

create table if not exists public.reflex_hosts (
  id uuid primary key,
  owner_user_id uuid not null references auth.users(id) on delete cascade,
  display_name text not null default 'Gaming PC',
  region text not null,
  agent_version text not null,
  hardware jsonb not null default '{}',
  hosting_enabled boolean not null default false,
  last_seen_at timestamptz,
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now()
);

create table if not exists public.reflex_host_game_offerings (
  id uuid primary key default gen_random_uuid(),
  host_id uuid not null references public.reflex_hosts(id) on delete cascade,
  game_id text not null references public.reflex_games(id),
  local_manifest_id text not null check (length(local_manifest_id) between 8 and 128),
  enabled boolean not null default true,
  verification_status text not null default 'pending'
    check (verification_status in ('pending', 'verified', 'missing', 'failed')),
  expected_process_names text[] not null default '{}',
  preferred_window_title text,
  last_verified_at timestamptz,
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now(),
  unique (host_id, game_id)
);

create table if not exists public.reflex_sessions (
  id uuid primary key,
  player_user_id uuid not null references auth.users(id),
  host_id uuid not null references public.reflex_hosts(id),
  offering_id uuid not null references public.reflex_host_game_offerings(id),
  game_id text not null references public.reflex_games(id),
  state text not null,
  requested_duration_seconds integer not null check (requested_duration_seconds between 300 and 28800),
  room_id text not null unique check (room_id ~ '^[a-f0-9]{32}$'),
  stream_profile jsonb not null default '{}',
  starts_at timestamptz,
  ends_at timestamptz,
  ended_at timestamptz,
  failure_code text,
  failure_detail text,
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now()
);

create table if not exists public.reflex_session_events (
  id bigint generated always as identity primary key,
  session_id uuid not null references public.reflex_sessions(id) on delete cascade,
  actor text not null check (actor in ('player', 'server', 'host')),
  event_type text not null,
  payload jsonb not null default '{}',
  created_at timestamptz not null default now()
);

create index if not exists reflex_offerings_game_enabled
  on public.reflex_host_game_offerings (game_id, enabled);
create index if not exists reflex_sessions_player_created
  on public.reflex_sessions (player_user_id, created_at desc);
create index if not exists reflex_session_events_session
  on public.reflex_session_events (session_id, created_at);

alter table public.reflex_games enable row level security;
alter table public.reflex_hosts enable row level security;
alter table public.reflex_host_game_offerings enable row level security;
alter table public.reflex_sessions enable row level security;
alter table public.reflex_session_events enable row level security;
