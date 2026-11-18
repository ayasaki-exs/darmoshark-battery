/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Darmoshark (MOJI) vendor HID protocols — the minimal battery slice.
 *
 * This header is shared by the kernel module, the userspace probe and the
 * host test, so there is exactly one C implementation of the frame layouts:
 *
 * 1. Nordic (1915) 64-byte non-numbered input/output report pair on usage page
 *    0xff0a, usage 0x0001, verified in OpenLogi (`darmoshark.rs`, branch
 *    `feat/darmoshark-driver`).
 *
 *    [0]      operation type; bit 6 (0x40) = 2.4 GHz work mode
 *    [1]      packet index / sub-index
 *    [2]      command opcode
 *    [3]      sub-command
 *    [4..62]  payload
 *    [63]     checksum (the whole report sums to 0xa1 mod 256)
 *
 * 2. Telink (248a) 21-byte feature report 0x51 on usage page 0x008c, usage
 *    0x0001 (`dms` contract), documented in `darmoshark-m3-configurator` and
 *    verified in SwarlexBattery.
 *
 * Keep it in sync with tests/protocol_test.c.
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

#define DS_TELINK_REPORT_ID 0x51
#define DS_TELINK_REPORT_LEN 21
#define DS_TELINK_OP_BOND 0x03
#define DS_TELINK_OP_IDENTIFY 0x06
#define DS_TELINK_STATUS_OK 1

/*
 * Build a 21-byte Telink feature report `[0x51, opcode, 0, ...]` on usage page
 * 0x008c. Opcode 0x06 queries identity and battery; opcode 0x03 reads receiver
 * bond info to prime the feature buffer before a wireless 0x06 read.
 */
static inline void darmoshark_build_telink_feature(ds_u8 *out, ds_u8 opcode)
{
	unsigned int i;

	for (i = 0; i < DS_TELINK_REPORT_LEN; i++)
		out[i] = 0;

	out[0] = DS_TELINK_REPORT_ID;
	out[1] = opcode;
}

/*
 * Whether a feature report buffer from report ID 0x51 echoes `opcode`.
 */
static inline int darmoshark_is_telink_echo(const ds_u8 *resp,
					    unsigned int len, ds_u8 opcode)
{
	return len >= 2 && resp[0] == DS_TELINK_REPORT_ID && resp[1] == opcode;
}

/*
 * Parse the battery percentage out of a Telink 0x51 identify (0x06) response:
 *   [0]      report ID (0x51)
 *   [1]      opcode echo (0x06)
 *   [2]      status (1 = ok)
 *   [4..5]   vendor ID (little-endian)
 *   [6..7]   product ID (little-endian)
 *   [8..9]   firmware version (little-endian)
 *   [11]     battery percentage (1..100; 0 means no reading)
 *
 * Returns 1 and stores the percentage on success, 0 otherwise.
 */
static inline int darmoshark_parse_telink_power(const ds_u8 *resp,
						unsigned int len,
						ds_u8 *percent)
{
	ds_u8 value;

	if (len < 12)
		return 0;
	if (!darmoshark_is_telink_echo(resp, len, DS_TELINK_OP_IDENTIFY))
		return 0;
	if (resp[2] != DS_TELINK_STATUS_OK)
		return 0;

	value = resp[11];
	if (value < 1 || value > 100)
		return 0;

	*percent = value;
	return 1;
}

#endif /* DARMOSHARK_PROTOCOL_H */
