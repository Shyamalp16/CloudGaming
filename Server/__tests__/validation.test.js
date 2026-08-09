const { validateRedisEnvelope, validateSignalingMessage } = require('../validation');

describe('validation module', () => {
  it('accepts valid offer', () => {
    const res = validateSignalingMessage({ type: 'offer', sdp: 'v=0' });
    expect(res.ok).toBe(true);
    expect(res.data).toEqual({ type: 'offer', sdp: 'v=0' });
  });

  it('accepts valid answer', () => {
    const res = validateSignalingMessage({ type: 'answer', sdp: 'v=0' });
    expect(res.ok).toBe(true);
    expect(res.data).toEqual({ type: 'answer', sdp: 'v=0' });
  });

  it('accepts valid candidate with optional fields', () => {
    const res = validateSignalingMessage({
      type: 'candidate',
      candidate: 'cand',
      sdpMid: '0',
      sdpMLineIndex: 0,
    });
    expect(res.ok).toBe(true);
    expect(res.data).toEqual({
      type: 'candidate',
      candidate: 'cand',
      sdpMid: '0',
      sdpMLineIndex: 0,
    });
  });

  it('accepts valid control with payload', () => {
    const res = validateSignalingMessage({ type: 'control', action: 'ping', payload: { x: 1 } });
    expect(res.ok).toBe(true);
    expect(res.data).toEqual({ type: 'control', action: 'ping', payload: { x: 1 } });
  });

  it('accepts a strict negotiated stream profile', () => {
    const res = validateSignalingMessage({
      type: 'stream-profile',
      sessionId: '01234567-89ab-4cde-8fab-0123456789ab',
      width: 1920,
      height: 1080,
      fps: 60,
      bitrate: 8000000,
      capabilities: {
        maxWidth: 2560,
        maxHeight: 1440,
        maxFps: 60,
        maxBitrate: 12000000,
        h264: true,
      },
    });
    expect(res.ok).toBe(true);
  });

  it('rejects unknown profile fields and missing capabilities', () => {
    expect(
      validateSignalingMessage({
        type: 'stream-profile',
        width: 1920,
        height: 1080,
        fps: 60,
        bitrate: 8000000,
      }).ok,
    ).toBe(false);
    expect(
      validateSignalingMessage({
        type: 'stream-profile',
        width: 1920,
        height: 1080,
        fps: 60,
        bitrate: 8000000,
        capabilities: {
          maxWidth: 2560,
          maxHeight: 1440,
          maxFps: 60,
          maxBitrate: 12000000,
          h264: true,
        },
        surprise: true,
      }).ok,
    ).toBe(false);
  });

  it('rejects missing fields', () => {
    const res = validateSignalingMessage({ type: 'offer' });
    expect(res.ok).toBe(false);
    expect(res.error).toBeDefined();
  });

  it('rejects invalid type', () => {
    const res = validateSignalingMessage({ type: 'bogus', sdp: 'v=0' });
    expect(res.ok).toBe(false);
  });

  it('rejects empty strings where disallowed', () => {
    const res = validateSignalingMessage({ type: 'candidate', candidate: '' });
    expect(res.ok).toBe(false);
  });

  it('strictly validates internal Redis fanout envelopes', () => {
    const envelope = {
      senderId: 'client:01234567-89ab-4cde-8fab-0123456789ab',
      senderRole: 'player',
      sessionId: '01234567-89ab-4cde-8fab-0123456789ab',
      hostId: '11111111-2222-4333-8444-555555555555',
      data: { type: 'offer', sessionId: '01234567-89ab-4cde-8fab-0123456789ab', sdp: 'v=0' },
      originServerId: 'aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee',
    };
    expect(validateRedisEnvelope(envelope).ok).toBe(true);
    expect(validateRedisEnvelope({ ...envelope, injected: true }).ok).toBe(false);
    expect(validateRedisEnvelope({ ...envelope, senderRole: 'admin' }).ok).toBe(false);
  });

  it('rejects excessively deep control payloads', () => {
    let payload = {};
    for (let index = 0; index < 12; index++) payload = { nested: payload };
    expect(validateSignalingMessage({ type: 'control', action: 'ping', payload }).ok).toBe(false);
  });
});
