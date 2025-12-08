const client = require('prom-client');

// Create a Registry to register the metrics
const register = new client.Registry();
client.collectDefaultMetrics({ register });

// Gauges
const gaugeActiveConnections = new client.Gauge({
	name: 'signaling_active_connections',
	help: 'Number of active WebSocket connections on this instance',
});
const gaugeLocalRooms = new client.Gauge({
	name: 'signaling_local_rooms',
	help: 'Number of rooms with at least one local connection on this instance',
});
const gaugeRedisUp = new client.Gauge({
	name: 'signaling_redis_up',
	help: 'Indicates if Redis connection is healthy (1 for up, 0 for down)',
});
const gaugeCircuitBreakerOpen = new client.Gauge({
	name: 'signaling_circuit_breaker_open',
	help: 'Indicates if the Redis circuit breaker is open (1 for open, 0 for closed)',
});

// Counters
const counterMessagesForwarded = new client.Counter({
	name: 'signaling_messages_forwarded_total',
	help: 'Total messages forwarded to clients',
});
const counterSchemaRejects = new client.Counter({
	name: 'signaling_schema_rejections_total',
	help: 'Total messages rejected due to schema validation',
});
const counterRateLimitDrops = new client.Counter({
	name: 'signaling_rate_limit_drops_total',
	help: 'Total messages dropped due to rate limiting',
});
const counterBackpressureCloses = new client.Counter({
	name: 'signaling_backpressure_closes_total',
	help: 'Total connections closed due to excessive backpressure',
});

// Histograms
const histoRedisLatency = new client.Histogram({
	name: 'signaling_redis_cmd_latency_seconds',
	help: 'Observed latency of Redis commands',
	buckets: [0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1],
});
const histoFanoutLatency = new client.Histogram({
	name: 'signaling_fanout_latency_seconds',
	help: 'Latency to fan out a message to local peers',
	buckets: [0.0005, 0.001, 0.005, 0.01, 0.025, 0.05],
});

// Event loop lag gauge (prom-client has it via collectDefaultMetrics)

register.registerMetric(gaugeActiveConnections);
register.registerMetric(gaugeLocalRooms);
register.registerMetric(gaugeRedisUp);
register.registerMetric(gaugeCircuitBreakerOpen);
register.registerMetric(counterMessagesForwarded);
register.registerMetric(counterSchemaRejects);
register.registerMetric(counterRateLimitDrops);
register.registerMetric(counterBackpressureCloses);
register.registerMetric(histoRedisLatency);
register.registerMetric(histoFanoutLatency);

function setActiveConnections(n) { gaugeActiveConnections.set(n); }
function setLocalRooms(n) { gaugeLocalRooms.set(n); }
function setRedisUp(isUp) { gaugeRedisUp.set(isUp ? 1 : 0); }
function setCircuitBreakerOpen(isOpen) { gaugeCircuitBreakerOpen.set(isOpen ? 1 : 0); }
function incMessagesForwarded(n = 1) { counterMessagesForwarded.inc(n); }
function incSchemaRejects(n = 1) { counterSchemaRejects.inc(n); }
function incRateLimitDrops(n = 1) { counterRateLimitDrops.inc(n); }
function incBackpressureCloses(n = 1) { counterBackpressureCloses.inc(n); }
function observeRedisLatency(seconds) { histoRedisLatency.observe(seconds); }
function observeFanoutLatency(seconds) { histoFanoutLatency.observe(seconds); }
function startRedisTimer() { return histoRedisLatency.startTimer(); }
function startFanoutTimer() { return histoFanoutLatency.startTimer(); }

async function metricsHandler(req, res) {
	try {
		res.setHeader('Content-Type', register.contentType);
		res.end(await register.metrics());
	} catch (e) {
		res.statusCode = 500;
		res.end('metrics_error');
	}
}

// Optional: allow tests to stop default metrics collection to avoid open handle warnings
let _stopDefaultMetrics = null;
try {
	const rv = client.collectDefaultMetrics({ register });
	if (typeof rv === 'function') _stopDefaultMetrics = rv;
} catch (_) {}

module.exports = {
	register,
	metricsHandler,
	setActiveConnections,
	setLocalRooms,
	setRedisUp,
	setCircuitBreakerOpen,
	incMessagesForwarded,
	incSchemaRejects,
	incRateLimitDrops,
	incBackpressureCloses,
	observeRedisLatency,
	observeFanoutLatency,
	startRedisTimer,
	startFanoutTimer,
	stopDefaultMetrics: () => { try { if (_stopDefaultMetrics) _stopDefaultMetrics(); } catch (_) {} },
};


