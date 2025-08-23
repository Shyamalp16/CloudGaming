// Atomic room join/leave using Lua to avoid races under contention
// join script:
// ARGV[1] = clientId, ARGV[2] = capacity
// Returns: new size if joined (>=1), 0 if already member, -1 if full (not joined)
const LUA_JOIN = `
local room = KEYS[1]
local clientId = ARGV[1]
local capacity = tonumber(ARGV[2])

-- If already member, just return current size
if redis.call('SISMEMBER', room, clientId) == 1 then
  return redis.call('SCARD', room)
end

local size = redis.call('SCARD', room)
if size >= capacity then
  return -1
end

redis.call('SADD', room, clientId)
local newsize = size + 1
-- Remove TTL if this is the first member
if newsize == 1 then
  pcall(redis.call, 'PERSIST', room)
end
return newsize
`;

// leave script:
// ARGV[1] = clientId, ARGV[2] = ttlSeconds
// Returns: resulting size after removal (>=0)
const LUA_LEAVE = `
local room = KEYS[1]
local clientId = ARGV[1]
local ttl = tonumber(ARGV[2])
redis.call('SREM', room, clientId)
local size = redis.call('SCARD', room)
if size == 0 then
  pcall(redis.call, 'EXPIRE', room, ttl)
end
return size
`;

async function atomicJoin(redis, roomKey, clientId, capacity) {
	// Use EVAL directly; for high throughput, scripts can be cached with SCRIPT LOAD if needed
	const res = await redis.eval(LUA_JOIN, { keys: [roomKey], arguments: [clientId, String(capacity)] });
	return Number(res);
}

async function atomicLeave(redis, roomKey, clientId, ttlSeconds) {
	const res = await redis.eval(LUA_LEAVE, { keys: [roomKey], arguments: [clientId, String(ttlSeconds)] });
	return Number(res);
}

module.exports = { atomicJoin, atomicLeave };


