const client = require('prom-client');

const register = new client.Registry();
const sessions = new client.Counter({
  name: 'market_sessions_total',
  help: 'Marketplace session outcomes',
  labelNames: ['outcome'],
  registers: [register],
});
const failovers = new client.Counter({
  name: 'market_session_failovers_total',
  help: 'Sessions reassigned to another host',
  labelNames: ['reason'],
  registers: [register],
});
const timeToReady = new client.Histogram({
  name: 'market_session_time_to_ready_seconds',
  help: 'Time from player request until the game is ready',
  buckets: [1, 2, 5, 10, 20, 30, 60, 120, 180],
  registers: [register],
});
const controlHosts = new client.Gauge({
  name: 'market_control_hosts_connected',
  help: 'Hosts with an active marketplace control channel',
  registers: [register],
});

module.exports = {
  sessionOutcome: (outcome) => sessions.inc({ outcome }),
  sessionFailover: (reason) => failovers.inc({ reason }),
  observeReady: (seconds) => timeToReady.observe(seconds),
  setControlHosts: (count) => controlHosts.set(count),
  metrics: async () => ({ contentType: register.contentType, body: await register.metrics() }),
};
