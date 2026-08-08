const crypto = require('crypto');

function encode(value) {
	return Buffer.from(value).toString('base64url');
}

function signPairingToken({ roomId, sessionId, expiresAt }, secret) {
	if (!secret || typeof secret !== 'string' || secret.length < 32) throw new Error('PAIRING_TOKEN_SECRET must be at least 32 characters');
	const payload = encode(JSON.stringify({ v: 1, roomId, sessionId, exp: expiresAt, jti: crypto.randomUUID() }));
	const signature = crypto.createHmac('sha256', secret).update(payload).digest('base64url');
	return `${payload}.${signature}`;
}

function verifyPairingToken(token, secret, now = Date.now()) {
	if (typeof token !== 'string' || token.length > 2048 || !secret) return null;
	const parts = token.split('.');
	if (parts.length !== 2) return null;
	const expected = crypto.createHmac('sha256', secret).update(parts[0]).digest();
	let actual;
	try { actual = Buffer.from(parts[1], 'base64url'); } catch (_) { return null; }
	if (expected.length !== actual.length || !crypto.timingSafeEqual(expected, actual)) return null;
	try {
		const payload = JSON.parse(Buffer.from(parts[0], 'base64url').toString('utf8'));
		if (payload.v !== 1 || typeof payload.roomId !== 'string' || typeof payload.sessionId !== 'string' ||
			typeof payload.jti !== 'string' || !/^[0-9a-f-]{36}$/i.test(payload.jti) ||
			!Number.isSafeInteger(payload.exp) || payload.exp <= now) return null;
		return payload;
	} catch (_) { return null; }
}

module.exports = { signPairingToken, verifyPairingToken };
