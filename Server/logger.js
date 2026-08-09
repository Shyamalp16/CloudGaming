const pino = require('pino');
const { config } = require('./config');

const redact = {
  paths: [
    'req.headers.authorization',
    'req.headers.cookie',
    'req.headers["sec-websocket-protocol"]',
    'req.url',
    'authorization',
    'token',
    'jwt',
    'password',
    'hostId',
    'roomId',
    'sessionId',
    'clientId',
    'allowedOrigins',
    'ip',
    'origin',
    'channel',
    'err.message',
    'err.stack',
    'error.message',
    'error.stack',
  ],
  censor: '[Redacted]',
};

const transport =
  config.prettyLogs && config.env !== 'production'
    ? {
        target: 'pino-pretty',
        options: {
          colorize: true,
          singleLine: true,
          translateTime: 'SYS:standard',
        },
      }
    : undefined;

const logger = pino(
  {
    level: config.logLevel,
    redact,
    base: {
      instanceId: process.env.INSTANCE_ID || process.pid,
    },
  },
  transport ? pino.transport(transport) : undefined,
);

function withContext(ctx = {}) {
  return logger.child(ctx);
}

module.exports = { logger, withContext };
