/*
 *
 *  Wireless daemon for Linux
 *
 *  Copyright (C) 2013-2014  Intel Corporation. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <stdint.h>
#include <stdbool.h>
#include <asm/byteorder.h>
#include <linux/types.h>

enum eapol_protocol_version {
	EAPOL_PROTOCOL_VERSION_2001	= 1,
	EAPOL_PROTOCOL_VERSION_2004	= 2,
};

/*
 * 802.1X-2010: Table 11-5—Descriptor Type value assignments
 * The WPA key type of 254 comes from somewhere else.  Seems it is a legacy
 * value that might still be used by older implementations
 */
enum eapol_descriptor_type {
	EAPOL_DESCRIPTOR_TYPE_RC4	= 1,
	EAPOL_DESCRIPTOR_TYPE_80211	= 2,
	EAPOL_DESCRIPTOR_TYPE_WPA	= 254,
};

enum eapol_key_descriptor_version {
	EAPOL_KEY_DESCRIPTOR_VERSION_HMAC_MD5_ARC4	= 1,
	EAPOL_KEY_DESCRIPTOR_VERSION_HMAC_SHA1_AES	= 2,
	EAPOL_KEY_DESCRIPTOR_VERSION_AES_128_CMAC_AES	= 3,
};

struct eapol_sm;

struct eapol_key {
	uint8_t protocol_version;
	uint8_t packet_type;
	__be16 packet_len;
	uint8_t descriptor_type;
#if defined(__LITTLE_ENDIAN_BITFIELD)
	bool key_mic:1;
	bool secure:1;
	bool error:1;
	bool request:1;
	bool encrypted_key_data:1;
	bool smk_message:1;
	uint8_t reserved2:2;
	uint8_t key_descriptor_version:3;
	bool key_type:1;
	uint8_t reserved1:2;
	bool install:1;
	bool key_ack:1;
#elif defined (__BIG_ENDIAN_BITFIELD)
	uint8_t reserved2:2;
	bool smk_message:1;
	bool encrypted_key_data:1;
	bool request:1;
	bool error:1;
	bool secure:1;
	bool key_mic:1;
	bool key_ack:1;
	bool install:1;
	uint8_t reserved1:2;
	bool key_type:1;
	uint8_t key_descriptor_version:3;
#else
#error  "Please fix <asm/byteorder.h>"
#endif

	__be16 key_length;
	__be64 key_replay_counter;
	uint8_t key_nonce[32];
	uint8_t eapol_key_iv[16];
	uint8_t key_rsc[8];
	uint8_t reserved[8];
	uint8_t key_mic_data[16];
	__be16 key_data_len;
	uint8_t key_data[0];
} __attribute__ ((packed));

typedef int (*eapol_tx_packet_func_t)(uint32_t ifindex, const uint8_t *aa_addr,
				const uint8_t *sta_addr,
				const struct eapol_key *ek);
typedef bool (*eapol_get_nonce_func_t)(uint8_t nonce[]);

bool eapol_calculate_mic(const uint8_t *kck, const struct eapol_key *frame,
				uint8_t *mic);
bool eapol_verify_mic(const uint8_t *kck, const struct eapol_key *frame);

uint8_t *eapol_decrypt_key_data(const uint8_t *kek,
				const struct eapol_key *frame,
				size_t *decrypted_size);

const struct eapol_key *eapol_key_validate(const uint8_t *frame, size_t len);

bool eapol_verify_ptk_1_of_4(const struct eapol_key *ek);
bool eapol_verify_ptk_2_of_4(const struct eapol_key *ek);
bool eapol_verify_ptk_3_of_4(const struct eapol_key *ek);
bool eapol_verify_ptk_4_of_4(const struct eapol_key *ek);

struct eapol_key *eapol_create_ptk_2_of_4(
				enum eapol_protocol_version protocol,
				enum eapol_key_descriptor_version version,
				uint64_t key_replay_counter,
				const uint8_t snonce[],
				size_t extra_len,
				const uint8_t *extra_data);

struct eapol_key *eapol_create_ptk_4_of_4(
				enum eapol_protocol_version protocol,
				enum eapol_key_descriptor_version version,
				uint64_t key_replay_counter);

void __eapol_rx_packet(uint32_t ifindex, const uint8_t *sta_addr,
			const uint8_t *aa_addr,
			const uint8_t *frame, size_t len);

void __eapol_set_tx_packet_func(eapol_tx_packet_func_t func);
void __eapol_set_get_nonce_func(eapol_get_nonce_func_t func);
void __eapol_set_protocol_version(enum eapol_protocol_version version);

struct eapol_sm *eapol_sm_new();
void eapol_sm_free(struct eapol_sm *sm);

void eapol_sm_set_sta_address(struct eapol_sm *sm, const uint8_t *sta_addr);
void eapol_sm_set_aa_address(struct eapol_sm *sm, const uint8_t *aa_addr);
void eapol_sm_set_pmk(struct eapol_sm *sm, const uint8_t *pmk);
void eapol_sm_set_ap_rsn(struct eapol_sm *sm, const uint8_t *rsn_ie,
				size_t len);
void eapol_sm_set_own_rsn(struct eapol_sm *sm, const uint8_t *rsn_ie,
				size_t len);
struct l_io *eapol_open_pae(uint32_t index);

void eapol_start(uint32_t ifindex, struct eapol_sm *sm);

bool eapol_init();
bool eapol_exit();
