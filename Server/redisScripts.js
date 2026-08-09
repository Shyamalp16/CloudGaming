// Atomic room join/leave using Lua to avoid races under contention
// join script:
// KEYS[1] = room members set, KEYS[2] = role ownership key, KEYS[3] = client lease key
// ARGV[1] = clientId, ARGV[2] = capacity, ARGV[3] = role-key TTL seconds,
// ARGV[4] = stable role owner (host ID or authenticated session ID),
// ARGV[5] = namespaced client lease key prefix
// Returns: new size if joined (>=1), -1 if full, -2 if role already occupied
const LUA_JOIN = `
local room = KEYS[1]
local roleKey = KEYS[2]
local clientId = ARGV[1]
local capacity = tonumber(ARGV[2])
local roleTtl = tonumber(ARGV[3])
local roleOwner = ARGV[4]
local leasePrefix = ARGV[5]

-- If already member, just return current size
if redis.call('SISMEMBER', room, clientId) == 1 then
  return redis.call('SCARD', room)
end

for _, member in ipairs(redis.call('SMEMBERS', room)) do
	if redis.call('EXISTS', leasePrefix .. member) == 0 then redis.call('SREM', room, member) end
end

local size = redis.call('SCARD', room)
local existingRole = redis.call('GET', roleKey)
if existingRole then
  local existingOwner, existingClient = string.match(existingRole, '^([^|]+)|(.+)$')
  if existingOwner ~= roleOwner then return -2 end
	if existingClient then
	  redis.call('SREM', room, existingClient)
	  redis.call('DEL', leasePrefix .. existingClient)
	end
end

size = redis.call('SCARD', room)
if size >= capacity then return -1 end
redis.call('SADD', room, clientId)
redis.call('SET', roleKey, roleOwner .. '|' .. clientId, 'EX', roleTtl)
redis.call('SET', KEYS[3], roleOwner, 'EX', roleTtl)
local newsize = size + 1
redis.call('EXPIRE', room, roleTtl)
return newsize
`;

// leave script:
// KEYS[1] = room members set, KEYS[2] = role ownership key, KEYS[3] = client lease key
// ARGV[1] = clientId, ARGV[2] = ttlSeconds, ARGV[3] = stable role owner
// Returns: resulting size after removal (>=0)
const LUA_LEAVE = `
local room = KEYS[1]
local clientId = ARGV[1]
local ttl = tonumber(ARGV[2])
redis.call('SREM', room, clientId)
redis.call('DEL', KEYS[3])
if redis.call('GET', KEYS[2]) == ARGV[3] .. '|' .. clientId then
  redis.call('DEL', KEYS[2])
end
local size = redis.call('SCARD', room)
if size == 0 then
  pcall(redis.call, 'EXPIRE', room, ttl)
end
return size
`;

async function atomicJoin(
  redis,
  roomKey,
  roleKey,
  leaseKey,
  clientId,
  capacity,
  roleTtlSeconds,
  roleOwner,
  leasePrefix,
) {
  // Use EVAL directly; for high throughput, scripts can be cached with SCRIPT LOAD if needed
  const res = await redis.eval(LUA_JOIN, {
    keys: [roomKey, roleKey, leaseKey],
    arguments: [clientId, String(capacity), String(roleTtlSeconds), roleOwner, leasePrefix],
  });
  return Number(res);
}

async function atomicLeave(redis, roomKey, roleKey, leaseKey, clientId, ttlSeconds, roleOwner) {
  const res = await redis.eval(LUA_LEAVE, {
    keys: [roomKey, roleKey, leaseKey],
    arguments: [clientId, String(ttlSeconds), roleOwner],
  });
  return Number(res);
}

module.exports = { atomicJoin, atomicLeave };
