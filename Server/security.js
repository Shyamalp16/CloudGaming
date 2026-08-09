const crypto = require('crypto');
const UUID_PATTERN = /^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i;

function secureEqual(a, b) {
  if (typeof a !== 'string' || typeof b !== 'string') return false;
  const left = Buffer.from(a);
  const right = Buffer.from(b);
  return left.length === right.length && crypto.timingSafeEqual(left, right);
}

function normalizedIp(value) {
  if (typeof value !== 'string') return '';
  return value.startsWith('::ffff:') ? value.slice(7) : value;
}

function isTrustedProxy(remoteAddress, trustedProxyIps) {
  const remote = normalizedIp(remoteAddress);
  if (!remote || !trustedProxyIps || trustedProxyIps.size === 0) return false;
  for (const candidate of trustedProxyIps) {
    if (normalizedIp(candidate) === remote) return true;
  }
  return false;
}

function requestIsSecure(request, trustedProxyIps) {
  if (request.socket && request.socket.encrypted === true) return true;
  const remote = request.socket && request.socket.remoteAddress;
  if (!isTrustedProxy(remote, trustedProxyIps)) return false;
  const forwarded = request.headers && request.headers['x-forwarded-proto'];
  return typeof forwarded === 'string' && forwarded.split(',')[0].trim().toLowerCase() === 'https';
}

function canonicalOrigin(value) {
  try {
    const parsed = new URL(value);
    if (
      !['http:', 'https:'].includes(parsed.protocol) ||
      parsed.username ||
      parsed.password ||
      parsed.pathname !== '/' ||
      parsed.search ||
      parsed.hash
    )
      return null;
    return parsed.origin;
  } catch (_) {
    return null;
  }
}

function originAllowed(origin, allowedOrigins) {
  const candidate = canonicalOrigin(origin);
  if (!candidate) return false;
  return allowedOrigins.some((value) => canonicalOrigin(value) === candidate);
}

function bearerToken(headers) {
  const header = headers && headers.authorization;
  if (typeof header !== 'string') return '';
  const match = /^Bearer ([A-Za-z0-9._~+/=-]{1,4096})$/.exec(header);
  return match ? match[1] : '';
}

function hostCredentialValid(config, hostId, credential) {
  if (typeof hostId !== 'string' || !UUID_PATTERN.test(hostId)) return false;
  if (config.hostCredentials && config.hostCredentials.size > 0) {
    return secureEqual(credential, config.hostCredentials.get(hostId.toLowerCase()));
  }
  if (config.env === 'production') return false;
  return (
    secureEqual(credential, config.hostSecret) || secureEqual(credential, config.hostSecretPrevious)
  );
}

function extractProtocolCredential(header, prefix) {
  if (typeof header !== 'string') return '';
  const items = header
    .split(',')
    .map((value) => value.trim())
    .filter((value) => value.startsWith(prefix));
  return items.length === 1 ? items[0].slice(prefix.length) : '';
}

function sha256(value) {
  return crypto.createHash('sha256').update(value).digest('hex');
}

module.exports = {
  bearerToken,
  canonicalOrigin,
  extractProtocolCredential,
  hostCredentialValid,
  isTrustedProxy,
  normalizedIp,
  originAllowed,
  requestIsSecure,
  secureEqual,
  sha256,
  UUID_PATTERN,
};
