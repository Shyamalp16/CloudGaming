const { RateLimiter } = require('../rateLimiter');

function createFakeRedis() {
    const store = new Map();
    return {
        async incr(key) {
            const val = (store.get(key)?.value || 0) + 1;
            store.set(key, { value: val, expireAt: store.get(key)?.expireAt || 0 });
            return val;
        },
        async expire(key, seconds) {
            const now = Date.now();
            const ttl = now + seconds * 1000;
            const current = store.get(key) || { value: 0 };
            store.set(key, { value: current.value, expireAt: ttl });
            return 1;
        },
        // helper to fast-forward time in tests
        _advance(ms) {
            const now = Date.now() + ms;
            for (const [k, v] of store.entries()) {
                if (v.expireAt && v.expireAt <= now) store.delete(k);
            }
        },
        _store: store,
    };
}

describe('RateLimiter', () => {
    it('allows up to limit within window and rejects beyond', async () => {
        const fake = createFakeRedis();
        const rl = RateLimiter(fake);
        const common = { namespace: 'ip', id: '1.2.3.4', limit: 3, periodSeconds: 10 };

        expect(await rl.allow(common)).toBe(true); // 1
        expect(await rl.allow(common)).toBe(true); // 2
        expect(await rl.allow(common)).toBe(true); // 3
        expect(await rl.allow(common)).toBe(false); // 4 -> reject
    });

    it('resets after window expiry', async () => {
        const fake = createFakeRedis();
        const rl = RateLimiter(fake);
        const common = { namespace: 'room', id: 'abc', limit: 1, periodSeconds: 1 };

        expect(await rl.allow(common)).toBe(true);
        expect(await rl.allow(common)).toBe(false);
        // advance time beyond 1s window
        fake._advance(1500);
        expect(await rl.allow(common)).toBe(true);
    });

    it('fails open on redis error', async () => {
        const broken = {
            async incr() { throw new Error('boom'); },
            async expire() { /* no-op */ },
        };
        const rl = RateLimiter(broken);
        const ok = await rl.allow({ namespace: 'ip', id: 'x', limit: 1, periodSeconds: 10 });
        expect(ok).toBe(true);
    });
});


