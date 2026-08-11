const jwt = require('jsonwebtoken');
const { createRemoteJWKSet, jwtVerify } = require('jose');
const { bearerToken } = require('./security');

let jwks;

async function playerId(request, config) {
  if (!config.enableAuth) return 'local-player';
  const token = bearerToken(request.headers);
  if (!token) return null;
  try {
    let payload;
    if (config.jwt.jwksUrl) {
      jwks ||= createRemoteJWKSet(new URL(config.jwt.jwksUrl));
      ({ payload } = await jwtVerify(token, jwks, {
        issuer: config.jwt.issuer,
        audience: config.jwt.audience,
        algorithms: [config.jwt.alg],
      }));
    } else {
      payload = jwt.verify(token, config.jwt.secret, {
        issuer: config.jwt.issuer,
        audience: config.jwt.audience,
        algorithms: [config.jwt.alg],
      });
    }
    return typeof payload.sub === 'string' && payload.sub.length <= 128 ? payload.sub : null;
  } catch (_) {
    return null;
  }
}

module.exports = { playerId };
