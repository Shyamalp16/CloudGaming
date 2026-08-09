const { signPairingToken, verifyPairingToken } = require('../sessionTokens');

describe('pairing session tokens', () => {
  const secret = 'a-production-grade-secret-with-32-chars';
  const claims = {
    roomId: '0123456789abcdef0123456789abcdef',
    sessionId: '01234567-89ab-4cde-8fab-0123456789ab',
    hostId: '11111111-2222-4333-8444-555555555555',
    expiresAt: 1_300_000,
  };

  it('round trips signed room and session claims', () => {
    const token = signPairingToken(claims, secret);
    expect(verifyPairingToken(token, secret, 1_000_000)).toEqual(
      expect.objectContaining({
        v: 2,
        role: 'player',
        roomId: claims.roomId,
        sessionId: claims.sessionId,
        hostId: claims.hostId,
        exp: claims.expiresAt,
        jti: expect.stringMatching(/^[0-9a-f-]{36}$/i),
      }),
    );
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

  it('rejects malformed claims and implausibly distant expiration', () => {
    expect(() => signPairingToken({ ...claims, roomId: 'public-room-name' }, secret)).toThrow(
      /Invalid/,
    );
    const distant = signPairingToken({ ...claims, expiresAt: 2_000_000 }, secret);
    expect(verifyPairingToken(distant, secret, 1_000_000)).toBeNull();
  });
});
