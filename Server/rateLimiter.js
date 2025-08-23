function makeKey(ns, id, period) {
	return `rl:${ns}:${id}:${period}`;
}

function RateLimiter(redis) {
	return {
		// allow N actions per periodSeconds for id under namespace
		async allow({ namespace, id, limit, periodSeconds }) {
			const key = makeKey(namespace, id, periodSeconds);
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


