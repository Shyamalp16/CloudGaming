/** @type {import('jest').Config} */
module.exports = {
	testEnvironment: 'node',
	roots: ['<rootDir>'],
	testMatch: ['**/__tests__/**/*.test.js'],
	collectCoverage: true,
	coverageDirectory: 'coverage',
};

