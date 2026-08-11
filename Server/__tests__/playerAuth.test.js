const jwt = require('jsonwebtoken');
const { playerId } = require('../playerAuth');

describe('player API authentication', () => {
  const secret = 's'.repeat(32);
  const config = {
    enableAuth: true,
    jwt: { issuer: 'reflex', audience: 'players', alg: 'HS256', secret },
  };

  it('uses a fixed local identity only when authentication is disabled', async () => {
    await expect(playerId({ headers: {} }, { enableAuth: false })).resolves.toBe('local-player');
  });

  it('verifies configured claims and subject', async () => {
    const token = jwt.sign({ sub: 'player-1' }, secret, {
      issuer: 'reflex',
      audience: 'players',
      algorithm: 'HS256',
    });
    await expect(playerId({ headers: { authorization: `Bearer ${token}` } }, config)).resolves.toBe(
      'player-1',
    );
    await expect(
      playerId({ headers: { authorization: 'Bearer invalid' } }, config),
    ).resolves.toBeNull();
  });
});
