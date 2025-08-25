const { validateSignalingMessage } = require('../validation');

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
        const res = validateSignalingMessage({ type: 'candidate', candidate: 'cand', sdpMid: '0', sdpMLineIndex: 0 });
        expect(res.ok).toBe(true);
        expect(res.data).toEqual({ type: 'candidate', candidate: 'cand', sdpMid: '0', sdpMLineIndex: 0 });
    });

    it('accepts valid control with payload', () => {
        const res = validateSignalingMessage({ type: 'control', action: 'ping', payload: { x: 1 } });
        expect(res.ok).toBe(true);
        expect(res.data).toEqual({ type: 'control', action: 'ping', payload: { x: 1 } });
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
});


