const base = require('./jest.config');

module.exports = {
  ...base,
  testMatch: [
    '**/__tests__/{breaker,config,health.server,logger,metrics,metrics.unit,rateLimiter,security,sessionTokens,validation}.test.js',
  ],
};
