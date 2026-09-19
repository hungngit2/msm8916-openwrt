/*
 * Minimal Qualcomm DIAG protocol framing: HDLC-like byte-stuffing with a
 * CRC-16/CCITT (poly 0x1021, refin/refout, init 0, final XOR 0xffff)
 * trailer, terminated by 0x7e. Transport-agnostic -- same framing is used
 * whether the underlying transport is a USB serial diag port or (as here)
 * a raw rpmsg character device.
 */
#ifndef DIAG_PROTO_H
#define DIAG_PROTO_H

#include <stdint.h>
#include <string.h>

#define DIAG_TRAILER 0x7e
#define DIAG_ESC     0x7d
#define DIAG_ESC_XOR 0x20

/* CRC-16/X-25 (same as used by PPP/HDLC): reflected in/out, init 0xffff,
 * xorout 0xffff, poly 0x1021 reflected = 0x8408. Empirically verified
 * against a real DIAG_VERNO_F response from the modem. */
static uint16_t diag_crc16(const uint8_t *data, size_t len) {
	uint16_t crc = 0xffff;
	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (int b = 0; b < 8; b++) {
			if (crc & 1)
				crc = (crc >> 1) ^ 0x8408;
			else
				crc >>= 1;
		}
	}
	return crc ^ 0xffff;
}

/* Encode `payload` (len bytes) into a framed DIAG packet in `out`
 * (caller-provided buffer, must be at least 2*len + 4 bytes).
 * Returns the number of bytes written. */
static size_t diag_frame_encode(const uint8_t *payload, size_t len,
				 uint8_t *out, size_t out_cap) {
	uint16_t crc = diag_crc16(payload, len);
	uint8_t withcrc[512];
	if (len + 2 > sizeof(withcrc))
		return 0;
	memcpy(withcrc, payload, len);
	withcrc[len]     = crc & 0xff;
	withcrc[len + 1] = (crc >> 8) & 0xff;

	size_t o = 0;
	for (size_t i = 0; i < len + 2; i++) {
		uint8_t b = withcrc[i];
		if (b == DIAG_TRAILER || b == DIAG_ESC) {
			if (o + 2 > out_cap) return 0;
			out[o++] = DIAG_ESC;
			out[o++] = b ^ DIAG_ESC_XOR;
		} else {
			if (o + 1 > out_cap) return 0;
			out[o++] = b;
		}
	}
	if (o + 1 > out_cap) return 0;
	out[o++] = DIAG_TRAILER;
	return o;
}

/* Decode a raw framed buffer (trailer-terminated, escaped) into `out`.
 * Returns payload length (excluding the 2-byte CRC, which is verified),
 * or -1 on framing/CRC error. */
static int diag_frame_decode(const uint8_t *in, size_t in_len,
			      uint8_t *out, size_t out_cap) {
	size_t o = 0;
	for (size_t i = 0; i < in_len; i++) {
		uint8_t b = in[i];
		if (b == DIAG_TRAILER)
			break;
		if (b == DIAG_ESC) {
			if (++i >= in_len) return -1;
			b = in[i] ^ DIAG_ESC_XOR;
		}
		if (o >= out_cap) return -1;
		out[o++] = b;
	}
	if (o < 2) return -1;
	size_t payload_len = o - 2;
	uint16_t got_crc = out[payload_len] | (out[payload_len + 1] << 8);
	uint16_t exp_crc = diag_crc16(out, payload_len);
	if (got_crc != exp_crc)
		return -1;
	return (int)payload_len;
}

#endif
