const base = require('./jest.config');

module.exports = {
  ...base,
  testMatch: [
    '**/__tests__/{breaker,config,health.server,logger,metrics,metrics.unit,orchestration,playerAuth,rateLimiter,security,sessionTokens,validation}.test.js',
  ],
};
