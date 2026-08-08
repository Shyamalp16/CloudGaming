const { z } = require('zod');

const SessionId = z.string().uuid().optional();
const OfferSchema = z.object({ type: z.literal('offer'), sessionId: SessionId, sdp: z.string().min(1).max(256 * 1024) }).strict();
const AnswerSchema = z.object({ type: z.literal('answer'), sessionId: SessionId, sdp: z.string().min(1).max(256 * 1024) }).strict();
const CandidateSchema = z.object({
	type: z.literal('candidate'), sessionId: SessionId,
	candidate: z.string().min(1).max(4096), sdpMid: z.string().max(64).optional(),
	sdpMLineIndex: z.number().int().min(0).max(64).optional(),
}).strict();
const ControlSchema = z.object({
	type: z.literal('control'), sessionId: SessionId,
	action: z.enum(['terminate', 'replace', 'profile-request', 'profile-accepted', 'profile-rejected', 'schema-error', 'session-ready', 'ping']),
	payload: z.record(z.unknown()).optional(),
}).strict();
const ProfileSchema = z.object({
	type: z.literal('stream-profile'), sessionId: SessionId,
	width: z.number().int().min(640).max(3840), height: z.number().int().min(360).max(2160),
	fps: z.number().int().min(30).max(120), bitrate: z.number().int().min(500000).max(50000000),
	capabilities: z.object({
		maxWidth: z.number().int().min(640).max(7680), maxHeight: z.number().int().min(360).max(4320),
		maxFps: z.number().int().min(30).max(240), maxBitrate: z.number().int().min(500000).max(100000000),
		h264: z.boolean(),
	}).strict(),
}).strict();
const SignalingMessageSchema = z.discriminatedUnion('type', [OfferSchema, AnswerSchema, CandidateSchema, ControlSchema, ProfileSchema]);

function validateSignalingMessage(message) {
	const result = SignalingMessageSchema.safeParse(message);
	return result.success ? { ok: true, data: result.data } : { ok: false, error: result.error.flatten() };
}
module.exports = { validateSignalingMessage };
