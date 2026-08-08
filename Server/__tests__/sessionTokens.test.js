const { signPairingToken, verifyPairingToken } = require('../sessionTokens');

describe('pairing session tokens', () => {
    const secret = 'a-production-grade-secret-with-32-chars';
    const claims = {
        roomId: 'room-0123456789abcdef',
        sessionId: '01234567-89ab-4cde-8fab-0123456789ab',
        expiresAt: 2_000_000,
    };

    it('round trips signed room and session claims', () => {
        const token = signPairingToken(claims, secret);
        expect(verifyPairingToken(token, secret, 1_000_000)).toEqual(expect.objectContaining({
            v: 1, roomId: claims.roomId, sessionId: claims.sessionId, exp: claims.expiresAt,
            jti: expect.stringMatching(/^[0-9a-f-]{36}$/i),
        }));
    });

    it('mints a unique credential identifier for every reconnect token', () => {
        const first = verifyPairingToken(signPairingToken(claims, secret), secret, 1_000_000);
        const second = verifyPairingToken(signPairingToken(claims, secret), secret, 1_000_000);
        expect(first.jti).not.toBe(second.jti);
    });

    it('rejects expiration, tampering, and the wrong secret', () => {
        const token = signPairingToken(claims, secret);
        expect(verifyPairingToken(token, secret, claims.expiresAt)).toBeNull();
        expect(verifyPairingToken(`${token}x`, secret, 1_000_000)).toBeNull();
        expect(verifyPairingToken(token, `${secret}x`, 1_000_000)).toBeNull();
    });

    it('requires a sufficiently strong signing secret', () => {
        expect(() => signPairingToken(claims, 'short')).toThrow(/32 characters/);
    });
});
