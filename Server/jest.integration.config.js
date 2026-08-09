const base = require('./jest.config');

module.exports = {
  ...base,
  collectCoverage: false,
  testMatch: ['**/__tests__/{auth,pairing-rotation,session-auth}.test.js'],
};
