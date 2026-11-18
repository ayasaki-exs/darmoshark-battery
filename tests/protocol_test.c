// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Host-side vectors for darmoshark_protocol.h.
 *
 * These are the same facts the Rust codec's tests assert in OpenLogi:
 *  - the handshake is `[1, 0, 129, 1]`, its checksum byte is 30 and the whole
 *    report sums to 0xa1;
 *  - the 2.4 GHz mode sets bit 6;
 *  - `dms` reads the percentage at resp[6], `dms_v2` at resp[11].
 */
#include <stdio.h>
#include <string.h>

#include "darmoshark_protocol.h"

static int failures;

#define CHECK(cond)                                                            \
	do {                                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
				#cond);                                        \
			failures++;                                            \
		}                                                              \
	} while (0)

int main(void)
{
	ds_u8 out[DS_REPORT_LEN];
	ds_u8 resp[DS_REPORT_LEN];
	ds_u8 telink[DS_TELINK_REPORT_LEN];
	ds_u8 percent = 0;
	unsigned int i;
	unsigned int sum;

	/* USB handshake. */
	darmoshark_build_power(out, 0);
	CHECK(out[0] == 1);
	CHECK(out[1] == 0);
	CHECK(out[2] == 129);
	CHECK(out[3] == 1);
	CHECK(out[DS_REPORT_LEN - 1] == 30);
	sum = 0;
	for (i = 0; i < DS_REPORT_LEN; i++)
		sum += out[i];
	CHECK((sum & 0xff) == DS_CHECKSUM_TARGET);

	/* 2.4 GHz through the receiver. */
	darmoshark_build_power(out, 1);
	CHECK(out[0] == (1 | DS_WIRELESS_FLAG));
	sum = 0;
	for (i = 0; i < DS_REPORT_LEN; i++)
		sum += out[i];
	CHECK((sum & 0xff) == DS_CHECKSUM_TARGET);

	/* Response matching masks the work-mode bit. */
	memset(resp, 0, sizeof(resp));
	resp[0] = 1;
	resp[3] = 1;
	CHECK(darmoshark_is_power_response(resp, DS_REPORT_LEN));
	resp[0] = 0x41;
	CHECK(darmoshark_is_power_response(resp, DS_REPORT_LEN));
	resp[0] = 4;
	CHECK(!darmoshark_is_power_response(resp, DS_REPORT_LEN));
	resp[0] = 1;
	CHECK(!darmoshark_is_power_response(resp, 3));

	/* `dms` layout: percentage at resp[6]. */
	memset(resp, 0, sizeof(resp));
	resp[5] = 1;
	resp[6] = 42;
	resp[7] = 2;
	resp[10] = 3;
	CHECK(darmoshark_parse_power(resp, DS_REPORT_LEN, &percent));
	CHECK(percent == 42);

	/* `dms_v2` layout: percentage at resp[11]. */
	memset(resp, 0, sizeof(resp));
	resp[6] = 0x15;
	resp[7] = 0x19;
	resp[10] = 3;
	resp[11] = 77;
	CHECK(darmoshark_parse_power(resp, DS_REPORT_LEN, &percent));
	CHECK(percent == 77);

	/* An impossible percentage and a short report are not readings. */
	memset(resp, 0, sizeof(resp));
	resp[6] = 200;
	CHECK(!darmoshark_parse_power(resp, DS_REPORT_LEN, &percent));
	CHECK(!darmoshark_parse_power(resp, 11, &percent));

	/* Telink bond primer command `[0x51, 0x03, 0, ...]`. */
	darmoshark_build_telink_feature(telink, DS_TELINK_OP_BOND);
	CHECK(telink[0] == DS_TELINK_REPORT_ID);
	CHECK(telink[1] == DS_TELINK_OP_BOND);
	for (i = 2; i < DS_TELINK_REPORT_LEN; i++)
		CHECK(telink[i] == 0);

	/* Telink identify/battery command `[0x51, 0x06, 0, ...]`. */
	darmoshark_build_telink_feature(telink, DS_TELINK_OP_IDENTIFY);
	CHECK(telink[0] == DS_TELINK_REPORT_ID);
	CHECK(telink[1] == DS_TELINK_OP_IDENTIFY);
	for (i = 2; i < DS_TELINK_REPORT_LEN; i++)
		CHECK(telink[i] == 0);

	/* Telink echo matching. */
	CHECK(darmoshark_is_telink_echo(telink, DS_TELINK_REPORT_LEN,
					DS_TELINK_OP_IDENTIFY));
	CHECK(!darmoshark_is_telink_echo(telink, DS_TELINK_REPORT_LEN,
					 DS_TELINK_OP_BOND));
	CHECK(!darmoshark_is_telink_echo(telink, 1, DS_TELINK_OP_IDENTIFY));
	telink[0] = 0x52;
	CHECK(!darmoshark_is_telink_echo(telink, DS_TELINK_REPORT_LEN,
					 DS_TELINK_OP_IDENTIFY));

	/* Telink valid identify response with 87% battery. */
	memset(telink, 0, sizeof(telink));
	telink[0] = DS_TELINK_REPORT_ID;
	telink[1] = DS_TELINK_OP_IDENTIFY;
	telink[2] = DS_TELINK_STATUS_OK;
	telink[4] = 0x8a;
	telink[5] = 0x24;
	telink[6] = 0x12;
	telink[7] = 0xff;
	telink[8] = 0x09;
	telink[9] = 0x02;
	telink[11] = 87;
	percent = 0;
	CHECK(darmoshark_parse_telink_power(telink, DS_TELINK_REPORT_LEN,
					    &percent));
	CHECK(percent == 87);

	/* Telink invalid cases: 0%, >100%, bad status, wrong opcode, short. */
	telink[11] = 0;
	CHECK(!darmoshark_parse_telink_power(telink, DS_TELINK_REPORT_LEN,
					     &percent));
	telink[11] = 101;
	CHECK(!darmoshark_parse_telink_power(telink, DS_TELINK_REPORT_LEN,
					     &percent));
	telink[11] = 87;
	telink[2] = 0;
	CHECK(!darmoshark_parse_telink_power(telink, DS_TELINK_REPORT_LEN,
					     &percent));
	telink[2] = DS_TELINK_STATUS_OK;
	telink[1] = DS_TELINK_OP_BOND;
	CHECK(!darmoshark_parse_telink_power(telink, DS_TELINK_REPORT_LEN,
					     &percent));
	telink[1] = DS_TELINK_OP_IDENTIFY;
	CHECK(!darmoshark_parse_telink_power(telink, 11, &percent));

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}

	printf("protocol vectors OK\n");
	return 0;
}
