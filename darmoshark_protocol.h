/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Darmoshark (MOJI) vendor HID protocol — the minimal battery slice.
 *
 * The wire format below is the one documented and verified in OpenLogi's
 * `crates/openlogi-device/src/darmoshark.rs` (branch `feat/darmoshark-driver`).
 * This header is shared by the kernel module, the userspace probe and the
 * host test, so there is exactly one C implementation of the frame layout.
 *
 * Keep it in sync with the Rust codec. tests/protocol_test.c pins the same
 * wire facts the Rust tests assert, so a drift shows up as a red test.
 *
 * Frame (64 bytes, non-numbered input/output report pair on usage page
 * 0xff0a, usage 0x0001):
 *
 *   [0]      operation type; bit 6 (0x40) = 2.4 GHz work mode
 *   [1]      packet index / sub-index
 *   [2]      command opcode
 *   [3]      sub-command
 *   [4..62]  payload
 *   [63]     checksum (the whole report sums to 0xa1 mod 256)
 */
#ifndef DARMOSHARK_PROTOCOL_H
#define DARMOSHARK_PROTOCOL_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef u8 ds_u8;
#else
#include <stdint.h>
typedef uint8_t ds_u8;
#endif

#define DS_REPORT_LEN 64
#define DS_CHECKSUM_TARGET 0xa1
#define DS_WIRELESS_FLAG 0x40

#define DS_TYPE_POWER 0x01
#define DS_OP_BASE_INFO 129
#define DS_POWER_SUB 1

/*
 * Build the battery / handshake query `[1, 0, 129, 1]` (plus the 2.4 GHz bit
 * for a receiver) and apply the checksum.
 */
static inline void darmoshark_build_power(ds_u8 *out, int wireless)
{
	unsigned int i;
	unsigned int sum = 0;

	for (i = 0; i < DS_REPORT_LEN; i++)
		out[i] = 0;

	out[0] = (ds_u8)(DS_TYPE_POWER | (wireless ? DS_WIRELESS_FLAG : 0));
	out[2] = DS_OP_BASE_INFO;
	out[3] = DS_POWER_SUB;

	for (i = 0; i < DS_REPORT_LEN - 1; i++)
		sum += out[i];
	out[DS_REPORT_LEN - 1] = (ds_u8)(DS_CHECKSUM_TARGET - (sum & 0xff));
}

/*
 * Whether a 64-byte input report is the answer to the power query. The device
 * echoes the operation type with the work-mode bit, so mask it off.
 */
static inline int darmoshark_is_power_response(const ds_u8 *resp,
					       unsigned int len)
{
	return len >= 4 && (resp[0] & 0x3f) == DS_TYPE_POWER &&
	       resp[3] == DS_POWER_SUB;
}

/*
 * Parse the battery percentage out of a power response.
 *
 * `dms_v2` moves the fields: the Rust codec detects it from resp[6]/resp[7]
 * and reads the percentage at resp[11] instead of resp[6]. A value above 100
 * is not a reading and must not update the power supply.
 *
 * Returns 1 and stores the percentage on success, 0 otherwise.
 */
static inline int darmoshark_parse_power(const ds_u8 *resp, unsigned int len,
					 ds_u8 *percent)
{
	int dms_v2;
	ds_u8 value;

	if (len < 12)
		return 0;

	dms_v2 = resp[6] == 0x15 && resp[7] == 0x19;
	value = dms_v2 ? resp[11] : resp[6];
	if (value > 100)
		return 0;

	*percent = value;
	return 1;
}

#endif /* DARMOSHARK_PROTOCOL_H */
