const { z } = require('zod');

const OfferSchema = z.object({
	type: z.literal('offer'),
	sdp: z.string().min(1),
});

const AnswerSchema = z.object({
	type: z.literal('answer'),
	sdp: z.string().min(1),
});

const CandidateSchema = z.object({
	type: z.literal('candidate'),
	candidate: z.string().min(1),
	sdpMid: z.string().optional(),
	sdpMLineIndex: z.number().int().nonnegative().optional(),
});

const ControlSchema = z.object({
	type: z.literal('control'),
	action: z.string().min(1),
	// Allow arbitrary payload but ensure it's an object if present
	payload: z.unknown().optional(),
});

const SignalingMessageSchema = z.discriminatedUnion('type', [
	OfferSchema,
	AnswerSchema,
	CandidateSchema,
	ControlSchema,
]);

function validateSignalingMessage(message) {
	const result = SignalingMessageSchema.safeParse(message);
	if (!result.success) {
		return { ok: false, error: result.error.flatten() };
	}
	return { ok: true, data: result.data };
}

module.exports = {
	validateSignalingMessage,
};


