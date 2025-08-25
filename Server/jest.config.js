/** @type {import('jest').Config} */
module.exports = {
	testEnvironment: 'node',
	roots: ['<rootDir>'],
	testMatch: ['**/__tests__/**/*.test.js'],
	collectCoverage: true,
	coverageDirectory: 'coverage',
	coveragePathIgnorePatterns: [
		'/node_modules/',
		'/__tests__/',
		'/obselete/',
		'/Client/',
		'/Host/',
		'/gortc_main/',
	],
	coverageThreshold: {
		global: {
			statements: 80,
			branches: 70,
			functions: 80,
			lines: 80,
		},
	},
	// Ensure CI does not hang on open handles
	forceExit: true,
	detectOpenHandles: true,
};

