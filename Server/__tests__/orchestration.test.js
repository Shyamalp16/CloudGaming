const { PresenceSchema, SessionRequestSchema, hostScore, newSession } = require('../orchestration');

const hostId = '123e4567-e89b-12d3-a456-426614174000';

describe('marketplace orchestration', () => {
  it('accepts compact Steam and manual host inventory', () => {
    const result = PresenceSchema.parse({
      hostId,
      state: 'idle',
      region: 'ca-east',
      agentVersion: '0.2.0',
      games: [
        {
          id: 'steam:730',
          source: 'steam',
          title: 'Counter-Strike 2',
          localManifestId: 'steam-730',
        },
        { id: 'manual:demo-game', source: 'manual', title: 'Demo', localManifestId: 'manual-demo' },
      ],
    });
    expect(result.games).toHaveLength(2);
    expect(result.capabilities.maxFps).toBe(60);
  });

  it('rejects unsafe game and manifest identifiers', () => {
    expect(() =>
      PresenceSchema.parse({
        hostId,
        region: 'ca-east',
        agentVersion: '1',
        games: [
          { id: '../../game', source: 'manual', title: 'Bad', localManifestId: 'C:\\game.exe' },
        ],
      }),
    ).toThrow();
  });

  it('builds bounded sessions and ranks measured regions first', () => {
    const request = SessionRequestSchema.parse({
      gameId: 'steam:730',
      durationSeconds: 900,
      probes: [{ region: 'toronto', rttMs: 12 }],
    });
    const session = newSession('player-1', request);
    expect(session.roomId).toMatch(/^[a-f0-9]{32}$/);
    expect(session.state).toBe('allocating');
    expect(hostScore({ network: { probeRegion: 'toronto', probeRttMs: 8 } }, request.probes)).toBe(
      20,
    );
    expect(hostScore({ network: { probeRegion: 'montreal', probeRttMs: 5 } }, request.probes)).toBe(
      200,
    );
  });
});
