const { createClient } = require('redis');
const { config } = require('./config');

// Simple sliding window rate limiter using Redis INCR + EXPIRE
// key: namespace:identifier:period
// returns true if allowed, false if limited

function makeKey(ns, id, period) {
	return `rl:${ns}:${id}:${period}`;
}

function RateLimiter(redis) {
	return {
		// allow N actions per periodSeconds for id under namespace
		async allow({ namespace, id, limit, periodSeconds }) {
			const key = makeKey(namespace, id, periodSeconds);
			const now = Date.now();
			try {
				const count = await redis.incr(key);
				if (count === 1) await redis.expire(key, periodSeconds);
				return count <= limit;
			} catch (e) {
				// On Redis error, be permissive but log at caller
				return true;
			}
		},
	};
}

module.exports = { RateLimiter };


