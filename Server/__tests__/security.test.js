const {
  bearerToken,
  extractProtocolCredential,
  hostCredentialValid,
  originAllowed,
  requestIsSecure,
} = require('../security');

describe('security boundary helpers', () => {
  it('requires exact origin scheme, host, and effective port', () => {
    const allowed = ['https://play.example.com', 'http://127.0.0.1:8000'];
    expect(originAllowed('https://play.example.com', allowed)).toBe(true);
    expect(originAllowed('http://play.example.com', allowed)).toBe(false);
    expect(originAllowed('https://play.example.com:444', allowed)).toBe(false);
    expect(originAllowed('https://play.example.com.attacker.test', allowed)).toBe(false);
    expect(originAllowed('null', allowed)).toBe(false);
  });

  it('trusts forwarded TLS only from an explicitly trusted proxy', () => {
    const headers = { 'x-forwarded-proto': 'https' };
    expect(requestIsSecure({ socket: { encrypted: true }, headers }, new Set())).toBe(true);
    expect(requestIsSecure({ socket: { remoteAddress: '203.0.113.9' }, headers }, new Set())).toBe(
      false,
    );
    expect(
      requestIsSecure(
        { socket: { remoteAddress: '::ffff:10.0.0.5' }, headers },
        new Set(['10.0.0.5']),
      ),
    ).toBe(true);
  });

  it('parses bounded credentials without accepting header injection', () => {
    expect(bearerToken({ authorization: 'Bearer abc+/==' })).toBe('abc+/==');
    expect(bearerToken({ authorization: 'Bearer good\r\nInjected: yes' })).toBe('');
    expect(
      extractProtocolCredential('cloud-gaming-v1, cg-pairing.payload.signature', 'cg-pairing.'),
    ).toBe('payload.signature');
    expect(
      extractProtocolCredential(
        'cloud-gaming-v1, cg-pairing.first, cg-pairing.second',
        'cg-pairing.',
      ),
    ).toBe('');
  });

  it('binds production host credentials to one stable host ID', () => {
    const hostId = '11111111-2222-4333-8444-555555555555';
    const secret = '0123456789abcdef0123456789abcdef';
    const config = { env: 'production', hostCredentials: new Map([[hostId, secret]]) };
    expect(hostCredentialValid(config, hostId, secret)).toBe(true);
    expect(hostCredentialValid(config, 'aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee', secret)).toBe(false);
  });
});
