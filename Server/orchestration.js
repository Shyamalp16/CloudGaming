const { randomBytes, randomUUID } = require('crypto');
const { z } = require('zod');

const GameId = z.string().regex(/^(?:steam:\d{1,10}|manual:[a-z0-9][a-z0-9-]{0,63})$/);
const HostState = z.enum([
  'idle',
  'reserved',
  'preparing',
  'ready',
  'streaming',
  'cleaning',
  'failed',
]);
const GameSchema = z
  .object({
    id: GameId,
    source: z.enum(['steam', 'manual']),
    title: z.string().trim().min(1).max(160),
    localManifestId: z.string().min(8).max(128),
    enabled: z.boolean().default(true),
  })
  .strict();

const PresenceSchema = z
  .object({
    hostId: z.string().uuid(),
    state: HostState.default('idle'),
    region: z
      .string()
      .trim()
      .regex(/^[A-Za-z0-9_-]{1,32}$/),
    games: z.array(GameSchema).max(512),
    agentVersion: z.string().trim().min(1).max(32),
    capabilities: z
      .object({
        gpu: z.string().trim().max(160).optional(),
        maxWidth: z.number().int().min(640).max(7680).default(1920),
        maxHeight: z.number().int().min(480).max(4320).default(1080),
        maxFps: z.number().int().min(30).max(240).default(60),
      })
      .strict()
      .default({}),
    network: z
      .object({
        probeRegion: z
          .string()
          .trim()
          .regex(/^[A-Za-z0-9_-]{1,32}$/),
        probeRttMs: z.number().min(0).max(5000),
      })
      .strict()
      .optional(),
  })
  .strict();

const SessionRequestSchema = z
  .object({
    gameId: GameId,
    durationSeconds: z
      .number()
      .int()
      .min(300)
      .max(8 * 60 * 60),
    streamProfile: z
      .object({
        width: z.number().int().min(640).max(3840).default(1920),
        height: z.number().int().min(480).max(2160).default(1080),
        fps: z.number().int().min(30).max(120).default(60),
      })
      .strict()
      .default({}),
    probes: z
      .array(
        z.object({
          region: z
            .string()
            .trim()
            .regex(/^[A-Za-z0-9_-]{1,32}$/),
          rttMs: z.number().min(0).max(5000),
        }),
      )
      .max(16)
      .default([]),
  })
  .strict();

const PRESENCE_SCRIPT = `
local previous = redis.call('GET', KEYS[1])
if previous then
  for _, game in ipairs(cjson.decode(previous).games or {}) do
    redis.call('SREM', ARGV[3] .. game.id, ARGV[1])
  end
end
local presence = cjson.decode(ARGV[2])
local leased = redis.call('EXISTS', KEYS[2]) == 1
if leased and presence.state == 'idle' then presence.state = 'reserved' end
presence.lastSeenAt = tonumber(ARGV[4])
redis.call('SET', KEYS[1], cjson.encode(presence), 'EX', ARGV[5])
if not leased and presence.state == 'idle' then
  for _, game in ipairs(presence.games or {}) do
    if game.enabled then redis.call('SADD', ARGV[3] .. game.id, ARGV[1]) end
  end
end
return presence.state
`;

const CLAIM_SCRIPT = `
local raw = redis.call('GET', KEYS[1])
if not raw or redis.call('EXISTS', KEYS[2]) == 1 then return nil end
local host = cjson.decode(raw)
if host.state ~= 'idle' then return nil end
local offered = false
for _, game in ipairs(host.games or {}) do
  if game.enabled and game.id == ARGV[2] then offered = true break end
end
if not offered then return nil end
host.state = 'reserved'
host.sessionId = ARGV[3]
for _, game in ipairs(host.games or {}) do
  redis.call('SREM', ARGV[1] .. game.id, ARGV[4])
end
redis.call('SET', KEYS[1], cjson.encode(host), 'EX', ARGV[6])
redis.call('SET', KEYS[2], ARGV[3], 'EX', ARGV[5])
redis.call('SET', KEYS[3], ARGV[7], 'EX', ARGV[8])
return cjson.encode(host)
`;

const RELEASE_SCRIPT = `
local raw = redis.call('GET', KEYS[1])
if redis.call('GET', KEYS[2]) ~= ARGV[2] then return 0 end
if not raw then
  redis.call('DEL', KEYS[2])
  return 1
end
local host = cjson.decode(raw)
host.state = 'idle'
host.sessionId = nil
for _, game in ipairs(host.games or {}) do
  if game.enabled then redis.call('SADD', ARGV[1] .. game.id, ARGV[3]) end
end
redis.call('SET', KEYS[1], cjson.encode(host), 'EX', ARGV[4])
redis.call('DEL', KEYS[2])
return 1
`;

function hostScore(host, probes) {
  const probe = probes.find((item) => item.region === host.network?.probeRegion);
  const latency = probe ? probe.rttMs + host.network.probeRttMs : 200;
  return latency + (host.failureCount || 0) * 25;
}

function newSession(playerId, request) {
  const createdAt = Date.now();
  return {
    id: randomUUID(),
    playerId,
    gameId: request.gameId,
    durationSeconds: request.durationSeconds,
    streamProfile: request.streamProfile,
    state: 'allocating',
    roomId: randomBytes(16).toString('hex'),
    probes: request.probes,
    attemptedHostIds: [],
    createdAt,
    updatedAt: createdAt,
  };
}

module.exports = {
  CLAIM_SCRIPT,
  GameId,
  GameSchema,
  HostState,
  PRESENCE_SCRIPT,
  PresenceSchema,
  RELEASE_SCRIPT,
  SessionRequestSchema,
  hostScore,
  newSession,
};
