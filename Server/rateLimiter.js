const crypto = require('crypto');

function makeKey(prefix, ns, id, period) {
  const safeNamespace =
    String(ns)
      .replace(/[^A-Za-z0-9_-]/g, '')
      .slice(0, 32) || 'default';
  const digest = crypto.createHash('sha256').update(String(id)).digest('hex');
  return `${prefix}rl:${safeNamespace}:${digest}:${period}`;
}

const INCREMENT_SCRIPT = `
local count = redis.call('INCR', KEYS[1])
if count == 1 then redis.call('EXPIRE', KEYS[1], ARGV[1]) end
return count
`;

function RateLimiter(redis, prefix = 'cg:v1:', failClosed = false) {
  const fallback = new Map();
  let lastSweep = 0;

  function allowFallback(key, limit, periodSeconds) {
    const now = Date.now();
    if (now - lastSweep > 60000) {
      for (const [candidate, entry] of fallback) {
        if (entry.expiresAt <= now) fallback.delete(candidate);
      }
      lastSweep = now;
    }
    const existing = fallback.get(key);
    if (!existing && fallback.size >= 10000) return false;
    const entry =
      !existing || existing.expiresAt <= now
        ? { count: 0, expiresAt: now + periodSeconds * 1000 }
        : existing;
    entry.count += 1;
    fallback.set(key, entry);
    return entry.count <= limit;
  }

  return {
    // allow N actions per periodSeconds for id under namespace
    async allow({ namespace, id, limit, periodSeconds }) {
      const key = makeKey(prefix, namespace, id, periodSeconds);
      try {
        const count = Number(
          await redis.eval(INCREMENT_SCRIPT, { keys: [key], arguments: [String(periodSeconds)] }),
        );
        return count <= limit;
      } catch (e) {
        if (failClosed) return false;
        // Redis outages must not disable abuse controls. The bounded local
        // fallback is per-process, so edge limits remain recommended too.
        return allowFallback(key, limit, periodSeconds);
      }
    },
  };
}

module.exports = { RateLimiter };
