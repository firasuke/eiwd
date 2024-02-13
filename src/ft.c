/*
 *
 *  Wireless daemon for Linux
 *
 *  Copyright (C) 2017-2019  Intel Corporation. All rights reserved.
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <ell/ell.h>

#include "src/ie.h"
#include "src/handshake.h"
#include "src/crypto.h"
#include "src/ft.h"
#include "src/mpdu.h"
#include "src/auth-proto.h"
#include "src/band.h"
#include "src/scan.h"
#include "src/util.h"
#include "src/netdev.h"
#include "src/module.h"
#include "src/offchannel.h"
#include "src/wiphy.h"

static const unsigned int FT_ONCHANNEL_TIME = 300u; /* ms */

static ft_tx_frame_func_t tx_frame = NULL;
static struct l_queue *info_list = NULL;

struct ft_info {
	uint32_t ifindex;
	uint8_t spa[6];
	uint8_t aa[6];
	uint8_t snonce[32];
	uint8_t mde[3];
	uint8_t *fte;
	uint8_t *authenticator_ie;
	uint8_t prev_bssid[6];
	uint32_t frequency;
	uint32_t ds_frequency;
	uint32_t offchannel_id;
	/* Status of Authenticate/Action frame response, or error (< 0) */
	int status;

	struct l_timeout *timeout;
	struct wiphy_radio_work_item work;

	struct ie_ft_info ft_info;

	bool onchannel : 1;
};

/*
 * Calculate the MIC field of the FTE and write it directly to that FTE,
 * assuming it was all zeros before.  See 12.8.4 and 12.8.5.
 */
static bool ft_calculate_fte_mic(struct handshake_state *hs, uint8_t seq_num,
				const uint8_t *rsne, const uint8_t *fte,
				const uint8_t *ric, uint8_t *out_mic)
{
	struct iovec iov[10];
	int iov_elems = 0;
	struct l_checksum *checksum;
	const uint8_t *kck = handshake_state_get_kck(hs);
	size_t kck_len = handshake_state_get_kck_len(hs);
	uint8_t zero_mic[24] = {};

	iov[iov_elems].iov_base = hs->spa;
	iov[iov_elems++].iov_len = 6;

	iov[iov_elems].iov_base = hs->aa;
	iov[iov_elems++].iov_len = 6;

	iov[iov_elems].iov_base = &seq_num;
	iov[iov_elems++].iov_len = 1;

	if (rsne) {
		iov[iov_elems].iov_base = (void *) rsne;
		iov[iov_elems++].iov_len = rsne[1] + 2;
	}

	iov[iov_elems].iov_base = hs->mde;
	iov[iov_elems++].iov_len = hs->mde[1] + 2;

	if (fte) {
		iov[iov_elems].iov_base = (void *) fte;
		iov[iov_elems++].iov_len = 4;

		iov[iov_elems].iov_base = zero_mic;
		iov[iov_elems++].iov_len = kck_len;

		iov[iov_elems].iov_base = (void *) (fte + 4 + kck_len);
		iov[iov_elems++].iov_len = fte[1] + 2 - 4 - kck_len;
	}

	if (ric) {
		iov[iov_elems].iov_base = (void *) ric;
		iov[iov_elems++].iov_len = ric[1] + 2;
	}

	if (kck_len == 16)
		checksum = l_checksum_new_cmac_aes(kck, kck_len);
	else
		checksum = l_checksum_new_hmac(L_CHECKSUM_SHA384, kck, kck_len);

	if (!checksum)
		return false;

	l_checksum_updatev(checksum, iov, iov_elems);
	l_checksum_get_digest(checksum, out_mic, kck_len);
	l_checksum_free(checksum);

	return true;
}

/*
 * Validate the FC, the addresses, Auth Type and authentication sequence
 * number of an FT Authentication Response frame, return status code, and
 * the start of the IE array (RSN, MD, FT, TI and RIC).
 * See 8.3.3.1 for the header and 8.3.3.11 for the body format.
 */
static bool ft_parse_authentication_resp_frame(const uint8_t *data, size_t len,
				const uint8_t *addr1, const uint8_t *addr2,
				const uint8_t *addr3, uint16_t auth_seq,
				uint16_t *out_status, const uint8_t **out_ies,
				size_t *out_ies_len)
{
	uint16_t status = 0;

	if (len < 30)
		return false;

	if (memcmp(data + 4, addr1, 6))
		return false;
	if (memcmp(data + 10, addr2, 6))
		return false;
	if (memcmp(data + 16, addr3, 6))
		return false;

	/* Check Authentication algorithm number is FT (2) */
	if (l_get_le16(data + 24) != 2)
		return false;

	if (l_get_le16(data + 26) != auth_seq)
		return false;

	if (auth_seq == 2 || auth_seq == 4)
		status = l_get_le16(data + 28);

	if (out_status)
		*out_status = status;

	if (status == 0 && out_ies) {
		*out_ies = data + 28;
		*out_ies_len = len - 28;
	}

	return true;
}

static bool ft_parse_associate_resp_frame(const uint8_t *frame, size_t frame_len,
				uint16_t *out_status, const uint8_t **rsne,
				const uint8_t **mde, const uint8_t **fte)
{
	const struct mmpdu_header *mpdu;
	const struct mmpdu_association_response *body;
	struct ie_tlv_iter iter;

	mpdu = mpdu_validate(frame, frame_len);
	if (!mpdu)
		return false;

	body = mmpdu_body(mpdu);

	ie_tlv_iter_init(&iter, body->ies, (const uint8_t *) mpdu + frame_len -
				body->ies);

	while (ie_tlv_iter_next(&iter)) {
		switch (ie_tlv_iter_get_tag(&iter)) {
		case IE_TYPE_RSN:
			if (*rsne)
				return false;

			*rsne = ie_tlv_iter_get_data(&iter) - 2;
			break;

		case IE_TYPE_MOBILITY_DOMAIN:
			if (*mde)
				return false;

			*mde = ie_tlv_iter_get_data(&iter) - 2;
			break;

		case IE_TYPE_FAST_BSS_TRANSITION:
			if (*fte)
				return false;

			*fte = ie_tlv_iter_get_data(&iter) - 2;
			break;
		}
	}

	*out_status = L_LE16_TO_CPU(body->status_code);

	return true;
}

static bool ft_verify_rsne(const uint8_t *rsne, const uint8_t *pmk_r0_name,
				const uint8_t *authenticator_ie)
{
	/*
	 * In an RSN, check for an RSNE containing the PMK-R0-Name and
	 * the remaining fields same as in the advertised RSNE.
	 *
	 * 12.8.3: "The RSNE shall be present only if dot11RSNAActivated
	 * is true. If present, the RSNE shall be set as follows:
	 * - Version field shall be set to 1.
	 * - PMKID Count field shall be set to 1.
	 * - PMKID List field shall be set to the value contained in the
	 *   first message of this sequence.
	 * - All other fields shall be identical to the contents of the
	 *   RSNE advertised by the AP in Beacon and Probe Response frames."
	 */

	struct ie_rsn_info msg2_rsne;

	if (!rsne)
		return false;

	if (ie_parse_rsne_from_data(rsne, rsne[1] + 2,
						&msg2_rsne) < 0)
		return false;

	if (msg2_rsne.num_pmkids != 1 ||
				memcmp(msg2_rsne.pmkids, pmk_r0_name, 16))
		return false;

	if (!handshake_util_ap_ie_matches(&msg2_rsne, authenticator_ie, false))
		return false;

	return true;
}

static int parse_ies(struct handshake_state *hs,
			const uint8_t *authenticator_ie,
			const uint8_t *ies, size_t ies_len,
			const uint8_t **mde_out,
			const uint8_t **fte_out)
{
	struct ie_tlv_iter iter;
	const uint8_t *rsne = NULL;
	const uint8_t *mde = NULL;
	const uint8_t *fte = NULL;
	bool is_rsn;

	ie_tlv_iter_init(&iter, ies, ies_len);

	while (ie_tlv_iter_next(&iter)) {
		switch (ie_tlv_iter_get_tag(&iter)) {
		case IE_TYPE_RSN:
			if (rsne)
				goto ft_error;

			rsne = ie_tlv_iter_get_data(&iter) - 2;
			break;

		case IE_TYPE_MOBILITY_DOMAIN:
			if (mde)
				goto ft_error;

			mde = ie_tlv_iter_get_data(&iter) - 2;
			break;

		case IE_TYPE_FAST_BSS_TRANSITION:
			if (fte)
				goto ft_error;

			fte = ie_tlv_iter_get_data(&iter) - 2;
			break;
		}
	}

	is_rsn = hs->supplicant_ie != NULL;

	if (is_rsn) {
		if (!ft_verify_rsne(rsne, hs->pmk_r0_name, authenticator_ie))
			goto ft_error;
	} else if (rsne)
		goto ft_error;

	if (mde_out)
		*mde_out = mde;

	if (fte_out)
		*fte_out = fte;

	return 0;

ft_error:
	return -EINVAL;
}

static bool ft_parse_fte(struct handshake_state *hs,
				const uint8_t *snonce,
				const uint8_t *fte,
				struct ie_ft_info *ft_info)
{
	/*
	 * In an RSN, check for an FT IE with the same R0KH-ID and the same
	 * SNonce that we sent, and check that the R1KH-ID and the ANonce
	 * are present.  Use them to generate new PMK-R1, PMK-R1-Name and PTK
	 * in handshake.c.
	 *
	 * 12.8.3: "The FTE shall be present only if dot11RSNAActivated is
	 * true. If present, the FTE shall be set as follows:
	 * - R0KH-ID shall be identical to the R0KH-ID provided by the FTO
	 *   in the first message.
	 * - R1KH-ID shall be set to the R1KH-ID of the target AP, from
	 *   dot11FTR1KeyHolderID.
	 * - ANonce shall be set to a value chosen randomly by the target AP,
	 *   following the recommendations of 11.6.5.
	 * - SNonce shall be set to the value contained in the first message
	 *   of this sequence.
	 * - All other fields shall be set to 0."
	 */
	uint8_t zeros[24] = {};
	uint32_t kck_len = handshake_state_get_kck_len(hs);

	if (!fte)
		return false;

	if (ie_parse_fast_bss_transition_from_data(fte, fte[1] + 2,
					kck_len, ft_info) < 0)
		return false;

	if (ft_info->mic_element_count != 0 ||
			memcmp(ft_info->mic, zeros, kck_len))
		return false;

	if (hs->r0khid_len != ft_info->r0khid_len ||
			memcmp(hs->r0khid, ft_info->r0khid,
				hs->r0khid_len) ||
			!ft_info->r1khid_present)
		return false;

	if (memcmp(ft_info->snonce, snonce, 32))
		return false;

	return true;
}

static bool mde_equal(const uint8_t *mde1, const uint8_t *mde2)
{
	if (!mde1 || !mde2)
		return false;

	/*
	 * Check for an MD IE identical to the one we sent in message 1
	 *
	 * 12.8.3: "The MDE shall contain the MDID and FT Capability and
	 * Policy fields. This element shall be the same as the MDE
	 * advertised by the target AP in Beacon and Probe Response frames."
	 */
	return memcmp(mde1, mde1, mde1[1] + 2) == 0;
}

static int ft_over_ds_parse_action_response(const uint8_t *frame,
					size_t frame_len,
					const uint8_t **spa_out,
					const uint8_t **aa_out,
					const uint8_t **ies_out,
					size_t *ies_len)
{
	struct mmpdu_header *hdr = (struct mmpdu_header *)frame;
	size_t hdr_len = mmpdu_header_len(hdr);
	uint16_t status;
	const uint8_t *aa;
	const uint8_t *spa;

	if (frame_len < hdr_len + 16)
		return -EINVAL;

	frame += hdr_len;
	frame_len -= hdr_len;

	/* Category FT */
	if (frame[0] != 6)
		return -EINVAL;

	/* FT Action */
	if (frame[1] != 2)
		return -EINVAL;

	spa = frame + 2;
	aa = frame + 8;

	if (memcmp(spa, hdr->address_1, 6))
		return -EINVAL;

	if (spa_out)
		*spa_out = spa;

	if (aa_out)
		*aa_out = aa;

	status = l_get_le16(frame + 14);
	if (status != 0)
		goto done;

	if (ies_out && ies_len) {
		*ies_out = frame + 16;
		*ies_len = frame_len - 16;
	}

done:
	return (int)status;
}

int __ft_rx_associate(uint32_t ifindex, const uint8_t *frame, size_t frame_len)
{
	struct netdev *netdev = netdev_find(ifindex);
	struct handshake_state *hs = netdev_get_handshake(netdev);
	uint32_t kck_len = handshake_state_get_kck_len(hs);
	const uint8_t *rsne = NULL;
	const uint8_t *mde = NULL;
	const uint8_t *fte = NULL;
	const uint8_t *sent_mde = hs->mde;
	bool is_rsn = hs->supplicant_ie != NULL;
	uint16_t out_status;

	if (!ft_parse_associate_resp_frame(frame, frame_len, &out_status, &rsne,
					&mde, &fte))
		return -EBADMSG;

	if (out_status != 0)
		return (int)out_status;

	/*
	 * During a transition in an RSN, check for an RSNE containing the
	 * PMK-R1-Name and the remaining fields same as in the advertised
	 * RSNE.
	 *
	 * 12.8.5: "The RSNE shall be present only if dot11RSNAActivated is
	 * true. If present, the RSNE shall be set as follows:
	 * - Version field shall be set to 1.
	 * - PMKID Count field shall be set to 1.
	 * - PMKID field shall contain the PMKR1Name
	 * - All other fields shall be identical to the contents of the RSNE
	 *   advertised by the target AP in Beacon and Probe Response frames."
	 */
	if (is_rsn) {
		struct ie_rsn_info msg4_rsne;

		if (!rsne)
			return -EBADMSG;

		if (ie_parse_rsne_from_data(rsne, rsne[1] + 2,
						&msg4_rsne) < 0)
			return -EBADMSG;

		if (msg4_rsne.num_pmkids != 1 ||
				memcmp(msg4_rsne.pmkids, hs->pmk_r1_name, 16))
			return -EBADMSG;

		if (!handshake_util_ap_ie_matches(&msg4_rsne,
							hs->authenticator_ie,
							false))
			return -EBADMSG;
	} else {
		if (rsne)
			return -EBADMSG;
	}

	/* An MD IE identical to the one we sent must be present */
	if (sent_mde && (!mde || memcmp(sent_mde, mde, sent_mde[1] + 2)))
		return -EBADMSG;

	/*
	 * An FT IE is required in an initial mobility domain
	 * association and re-associations in an RSN but not present
	 * in a non-RSN (12.4.2 vs. 12.4.3).
	 */
	if (sent_mde && is_rsn && !fte)
		return -EBADMSG;
	if (!(sent_mde && is_rsn) && fte)
		return -EBADMSG;

	if (fte) {
		struct ie_ft_info ft_info;
		uint8_t mic[24];

		if (ie_parse_fast_bss_transition_from_data(fte, fte[1] + 2,
						kck_len, &ft_info) < 0)
			return -EBADMSG;

		/*
		 * In an RSN, check for an FT IE with the same
		 * R0KH-ID, R1KH-ID, ANonce and SNonce that we
		 * received in message 2, MIC Element Count
		 * of 6 and the correct MIC.
		 */

		if (!ft_calculate_fte_mic(hs, 6, rsne, fte, NULL, mic))
			return -EBADMSG;

		if (ft_info.mic_element_count != 3 ||
				memcmp(ft_info.mic, mic, kck_len))
			return -EBADMSG;

		if (hs->r0khid_len != ft_info.r0khid_len ||
				memcmp(hs->r0khid, ft_info.r0khid,
					hs->r0khid_len) ||
				!ft_info.r1khid_present ||
				memcmp(hs->r1khid, ft_info.r1khid, 6))
			return -EBADMSG;

		if (memcmp(ft_info.anonce, hs->anonce, 32))
			return -EBADMSG;

		if (memcmp(ft_info.snonce, hs->snonce, 32))
			return -EBADMSG;

		if (ft_info.gtk_len) {
			uint8_t gtk[32];

			if (!handshake_decode_fte_key(hs, ft_info.gtk,
							ft_info.gtk_len,
							gtk))
				return -EBADMSG;

			if (ft_info.gtk_rsc[6] != 0x00 ||
					ft_info.gtk_rsc[7] != 0x00)
				return -EBADMSG;

			handshake_state_install_gtk(hs, ft_info.gtk_key_id,
							gtk, ft_info.gtk_len,
							ft_info.gtk_rsc, 6);
		}

		if (ft_info.igtk_len) {
			uint8_t igtk[16];

			if (!handshake_decode_fte_key(hs, ft_info.igtk,
						ft_info.igtk_len, igtk))
				return -EBADMSG;

			handshake_state_install_igtk(hs, ft_info.igtk_key_id,
						igtk, ft_info.igtk_len,
						ft_info.igtk_ipn);
		}

		handshake_state_install_ptk(hs);
	}

	return 0;
}

static bool ft_build_authenticate_ies(struct handshake_state *hs, bool ocvc,
				const uint8_t *new_snonce, uint8_t *buf,
				size_t *len)
{
	uint32_t kck_len = handshake_state_get_kck_len(hs);
	bool is_rsn = hs->supplicant_ie != NULL;
	uint8_t *ptr = buf;

	if (is_rsn) {
		struct ie_rsn_info rsn_info;

		/*
		 * Rebuild the RSNE to include the PMKR0Name and append
		 * MDE + FTE.
		 *
		 * 12.8.2: "If present, the RSNE shall be set as follows:
		 * - Version field shall be set to 1.
		 * - PMKID Count field shall be set to 1.
		 * - PMKID List field shall contain the PMKR0Name.
		 * - All other fields shall be as specified in 8.4.2.27
		 *   and 11.5.3."
		 */
		if (ie_parse_rsne_from_data(hs->supplicant_ie,
						hs->supplicant_ie[1] + 2,
						&rsn_info) < 0)
			return false;

		rsn_info.num_pmkids = 1;
		rsn_info.pmkids = hs->pmk_r0_name;
		rsn_info.ocvc = ocvc;

		ie_build_rsne(&rsn_info, ptr);
		ptr += ptr[1] + 2;
	}

	/* The MDE advertised by the BSS must be passed verbatim */
	ptr[0] = IE_TYPE_MOBILITY_DOMAIN;
	ptr[1] = 3;
	memcpy(ptr + 2, hs->mde + 2, 3);
	ptr += 5;

	if (is_rsn) {
		struct ie_ft_info ft_info;

		/*
		 * 12.8.2: "If present, the FTE shall be set as follows:
		 * - R0KH-ID shall be the value of R0KH-ID obtained by the
		 *   FTO during its FT initial mobility domain association
		 *   exchange.
		 * - SNonce shall be set to a value chosen randomly by the
		 *   FTO, following the recommendations of 11.6.5.
		 * - All other fields shall be set to 0."
		 */

		memset(&ft_info, 0, sizeof(ft_info));

		memcpy(ft_info.r0khid, hs->r0khid, hs->r0khid_len);
		ft_info.r0khid_len = hs->r0khid_len;

		memcpy(ft_info.snonce, new_snonce, 32);

		ie_build_fast_bss_transition(&ft_info, kck_len, ptr);

		ptr += ptr[1] + 2;
	}

	if (len)
		*len = ptr - buf;

	return true;
}

void __ft_set_tx_frame_func(ft_tx_frame_func_t func)
{
	tx_frame = func;
}

static bool ft_parse_ies(struct ft_info *info, struct handshake_state *hs,
			const uint8_t *ies, size_t ies_len)
{
	const uint8_t *mde = NULL;
	const uint8_t *fte = NULL;
	bool is_rsn = hs->supplicant_ie != NULL;

	if (parse_ies(hs, info->authenticator_ie, ies, ies_len,
				&mde, &fte) < 0)
		return false;

	if (!mde_equal(info->mde, mde))
		goto ft_error;

	if (is_rsn) {
		if (!ft_parse_fte(hs, info->snonce, fte, &info->ft_info))
			goto ft_error;

		info->fte = l_memdup(fte, fte[1] + 2);
	} else if (fte)
		goto ft_error;

	return true;

ft_error:
	return false;
}

static struct ft_info *ft_info_find(uint32_t ifindex, const uint8_t *aa)
{
	const struct l_queue_entry *e;

	for (e = l_queue_get_entries(info_list); e; e = e->next) {
		struct ft_info *info = e->data;

		if (info->ifindex != ifindex)
			continue;

		if (aa && memcmp(info->aa, aa, 6))
			continue;

		return info;
	}

	return NULL;
}

void __ft_rx_action(uint32_t ifindex, const uint8_t *frame, size_t frame_len)
{
	struct netdev *netdev = netdev_find(ifindex);
	struct handshake_state *hs = netdev_get_handshake(netdev);
	struct ft_info *info;
	int ret;
	const uint8_t *aa = NULL;
	const uint8_t *spa = NULL;
	const uint8_t *ies = NULL;
	size_t ies_len = 0;

	ret = ft_over_ds_parse_action_response(frame, frame_len, &spa, &aa,
						&ies, &ies_len);
	if (ret < 0) {
		l_debug("Could not parse action response");
		return;
	}

	info = ft_info_find(ifindex, aa);
	if (!info) {
		l_debug("No FT info found for BSS "MAC, MAC_STR(aa));
		return;
	}


	if (ret != 0) {
		l_debug("BSS "MAC" rejected FT action with status=%u",
				MAC_STR(info->aa), ret);
		info->status = ret;
		goto done;
	}

	if (!ft_parse_ies(info, hs, ies, ies_len)) {
		l_debug("Could not parse action response IEs");
		goto ft_error;
	}

	info->status = ret;

done:
	l_timeout_remove(info->timeout);
	info->timeout = NULL;

	wiphy_radio_work_done(netdev_get_wiphy(netdev), info->work.id);

	return;

ft_error:
	l_debug("FT-over-DS authenticate to "MAC" failed", MAC_STR(info->aa));
}

static struct ft_info *ft_info_new(struct handshake_state *hs,
					const struct scan_bss *target_bss)
{
	struct ft_info *info = l_new(struct ft_info, 1);

	info->ifindex = hs->ifindex;
	memcpy(info->spa, hs->spa, 6);
	memcpy(info->aa, target_bss->addr, 6);
	memcpy(info->mde, target_bss->mde, sizeof(info->mde));
	memcpy(info->prev_bssid, hs->aa, 6);

	info->frequency = target_bss->frequency;

	if (target_bss->rsne)
		info->authenticator_ie = l_memdup(target_bss->rsne,
						target_bss->rsne[1] + 2);

	l_getrandom(info->snonce, 32);
	info->status = -ENOENT;

	return info;
}

static void ft_info_destroy(void *data)
{
	struct ft_info *info = data;

	if (info->fte)
		l_free(info->fte);

	if (info->authenticator_ie)
		l_free(info->authenticator_ie);

	if (info->timeout)
		l_timeout_remove(info->timeout);

	l_free(info);
}

static bool ft_prepare_handshake(struct ft_info *info,
					struct handshake_state *hs)
{
	uint32_t kck_len = handshake_state_get_kck_len(hs);
	struct ie_rsn_info rsn_info;
	struct ie_ft_info ft_info;
	uint8_t *fte;
	uint8_t *rsne;

	handshake_state_set_authenticator_address(hs, info->aa);

	memcpy(hs->mde + 2, info->mde, 3);

	handshake_state_set_chandef(hs, NULL);

	if (!hs->supplicant_ie)
		return true;

	if (info->authenticator_ie)
		handshake_state_set_authenticator_ie(hs,
							info->authenticator_ie);

	memcpy(hs->snonce, info->snonce, sizeof(hs->snonce));

	handshake_state_set_authenticator_fte(hs, info->fte);

	handshake_state_set_anonce(hs, info->ft_info.anonce);

	handshake_state_set_kh_ids(hs, info->ft_info.r0khid,
						info->ft_info.r0khid_len,
						info->ft_info.r1khid);

	handshake_state_derive_ptk(hs);

	/*
	 * Rebuild the RSNE to include the PMKR1Name and append
	 * MDE + FTE.
	 *
	 * 12.8.4: "If present, the RSNE shall be set as follows:
	 * - Version field shall be set to 1.
	 * - PMKID Count field shall be set to 1.
	 * - PMKID field shall contain the PMKR1Name.
	 * - All other fields shall be as specified in 8.4.2.27
	 *   and 11.5.3."
	 */
	if (ie_parse_rsne_from_data(hs->supplicant_ie,
					hs->supplicant_ie[1] + 2,
					&rsn_info) < 0)
		return false;

	rsn_info.num_pmkids = 1;
	rsn_info.pmkids = hs->pmk_r1_name;
	/* Always set OCVC false for FT for now */
	rsn_info.ocvc = false;
	rsne = alloca(256);

	ie_build_rsne(&rsn_info, rsne);
	handshake_state_set_supplicant_ie(hs, rsne);

	/*
	 * 12.8.4: "If present, the FTE shall be set as follows:
	 * - ANonce, SNonce, R0KH-ID, and R1KH-ID shall be set to
	 *   the values contained in the second message of this
	 *   sequence.
	 * - The Element Count field of the MIC Control field shall
	 *   be set to the number of elements protected in this
	 *   frame (variable).
	 * [...]
	 * - All other fields shall be set to 0."
	 */
	memset(&ft_info, 0, sizeof(ft_info));
	ft_info.mic_element_count = 3;
	memcpy(ft_info.r0khid, hs->r0khid, hs->r0khid_len);
	ft_info.r0khid_len = hs->r0khid_len;
	memcpy(ft_info.r1khid, hs->r1khid, 6);
	ft_info.r1khid_present = true;
	memcpy(ft_info.anonce, hs->anonce, 32);
	memcpy(ft_info.snonce, hs->snonce, 32);

	/*
	 * IEEE 802.11-2020 Section 13.7.1 FT reassociation in an RSN
	 *
	 * "If dot11RSNAOperatingChannelValidationActivated is true and
	 *  the FTO indicates OCVC capability, the target AP shall
	 *  ensure that OCI subelement of the FTE matches by ensuring
	 *  that all of the following are true:
	 *      - OCI subelement is present
	 *      - Channel information in the OCI matches current
	 *        operating channel parameters (see 12.2.9)"
	 */
	if (hs->supplicant_ocvc && hs->chandef) {
		oci_from_chandef(hs->chandef, ft_info.oci);
		ft_info.oci_present = true;
	}

	fte = alloca(256);
	ie_build_fast_bss_transition(&ft_info, kck_len, fte);

	if (!ft_calculate_fte_mic(hs, 5, rsne, fte, NULL, ft_info.mic))
		return false;

	/* Rebuild the FT IE now with the MIC included */
	ie_build_fast_bss_transition(&ft_info, kck_len, fte);

	handshake_state_set_supplicant_fte(hs, fte);

	return true;
}

static bool ft_send_action(struct wiphy_radio_work_item *work)
{
	struct ft_info *info = l_container_of(work, struct ft_info, work);
	struct netdev *netdev = netdev_find(info->ifindex);
	struct handshake_state *hs = netdev_get_handshake(netdev);
	uint8_t ft_req[14];
	struct iovec iov[5];
	uint8_t ies[512];
	size_t len;
	int ret = -EINVAL;

	ft_req[0] = 6; /* FT category */
	ft_req[1] = 1; /* FT Request action */
	memcpy(ft_req + 2, info->spa, 6);
	memcpy(ft_req + 8, info->aa, 6);

	l_debug("");

	if (!ft_build_authenticate_ies(hs, hs->supplicant_ocvc, info->snonce,
					ies, &len))
		goto failed;

	iov[0].iov_base = ft_req;
	iov[0].iov_len = sizeof(ft_req);

	iov[1].iov_base = ies;
	iov[1].iov_len = len;

	ret = tx_frame(hs->ifindex, 0x00d0, info->ds_frequency, hs->aa, iov, 2);
	if (ret < 0)
		goto failed;

	l_queue_push_tail(info_list, info);

	return false;

failed:
	l_debug("FT-over-DS action failed to "MAC, MAC_STR(hs->aa));

	ft_info_destroy(info);
	return true;
}

struct wiphy_radio_work_item_ops ft_ops = {
	.do_work = ft_send_action,
};

static void ft_ds_timeout(struct l_timeout *timeout, void *user_data)
{
	struct ft_info *info = user_data;
	struct netdev *netdev = netdev_find(info->ifindex);

	wiphy_radio_work_done(netdev_get_wiphy(netdev), info->work.id);
}

int ft_action(uint32_t ifindex, uint32_t freq, const struct scan_bss *target)
{
	struct netdev *netdev = netdev_find(ifindex);
	struct handshake_state *hs = netdev_get_handshake(netdev);
	struct ft_info *info;

	info = ft_info_new(hs, target);
	info->ds_frequency = freq;
	info->timeout = l_timeout_create_ms(200, ft_ds_timeout, info, NULL);

	wiphy_radio_work_insert(netdev_get_wiphy(netdev), &info->work,
				WIPHY_WORK_PRIORITY_FT, &ft_ops);

	return 0;
}

void __ft_rx_authenticate(uint32_t ifindex, const uint8_t *frame,
				size_t frame_len)
{
	struct netdev *netdev = netdev_find(ifindex);
	struct handshake_state *hs = netdev_get_handshake(netdev);
	struct ft_info *info;
	uint16_t status;
	const uint8_t *ies;
	size_t ies_len;

	info = ft_info_find(ifindex, NULL);
	if (!info)
		return;

	if (!ft_parse_authentication_resp_frame(frame, frame_len,
					info->spa, info->aa, info->aa, 2,
					&status, &ies, &ies_len)) {
		l_debug("Could not parse auth response");
		return;
	}

	if (status != 0) {
		l_debug("BSS "MAC" rejected FT auth with status=%u",
				MAC_STR(info->aa), status);
		info->status = status;
		goto cancel;
	}

	if (!ft_parse_ies(info, hs, ies, ies_len)) {
		l_debug("Could not parse auth response IEs");
		goto cancel;
	}

	info->status = status;

cancel:
	/*
	 * Verified to be expected target, offchannel or onchannel work can
	 * now be canceled
	 */
	if (info->onchannel) {
		l_timeout_remove(info->timeout);
		info->timeout = NULL;
		wiphy_radio_work_done(netdev_get_wiphy(netdev), info->work.id);
	} else
		offchannel_cancel(netdev_get_wdev_id(netdev),
							info->offchannel_id);
}

static void ft_send_authenticate(void *user_data)
{
	struct ft_info *info = user_data;
	struct netdev *netdev = netdev_find(info->ifindex);
	struct handshake_state *hs = netdev_get_handshake(netdev);
	uint8_t ies[256];
	size_t len;
	struct iovec iov[2];
	struct mmpdu_authentication auth;

	l_debug("");

	/* Authentication body */
	auth.algorithm = L_CPU_TO_LE16(MMPDU_AUTH_ALGO_FT);
	auth.transaction_sequence = L_CPU_TO_LE16(1);
	auth.status = L_CPU_TO_LE16(0);

	iov[0].iov_base = &auth;
	iov[0].iov_len = sizeof(struct mmpdu_authentication);

	if (!ft_build_authenticate_ies(hs, hs->supplicant_ocvc, info->snonce,
					ies, &len))
		return;

	iov[1].iov_base = ies;
	iov[1].iov_len = len;

	tx_frame(info->ifindex, 0x00b0, info->frequency, info->aa, iov, 2);
}

static void ft_authenticate_destroy(int error, void *user_data)
{
	struct ft_info *info = user_data;

	info->offchannel_id = 0;
}

/*
 * There is no callback here because its assumed that another work item will
 * be inserted following this call which will check if authentication succeeded
 * via ft_handshake_setup.
 *
 * If the netdev goes away while authentication is in-flight station will clear
 * the authentications during cleanup, and in turn cancel the offchannel
 * request.
 */
int ft_authenticate(uint32_t ifindex, const struct scan_bss *target)
{
	struct netdev *netdev = netdev_find(ifindex);
	struct handshake_state *hs = netdev_get_handshake(netdev);
	struct ft_info *info = ft_info_new(hs, target);

	info->offchannel_id = offchannel_start(netdev_get_wdev_id(netdev),
						WIPHY_WORK_PRIORITY_FT,
						target->frequency,
						200, ft_send_authenticate, info,
						ft_authenticate_destroy);

	l_queue_push_tail(info_list, info);

	return 0;
}

static void ft_onchannel_timeout(struct l_timeout *timeout, void *user_data)
{
	struct ft_info *info = user_data;
	struct netdev *netdev = netdev_find(info->ifindex);

	wiphy_radio_work_done(netdev_get_wiphy(netdev), info->work.id);
}

static bool ft_send_authenticate_onchannel(struct wiphy_radio_work_item *work)
{
	struct ft_info *info = l_container_of(work, struct ft_info, work);

	ft_send_authenticate(info);

	info->timeout = l_timeout_create_ms(FT_ONCHANNEL_TIME,
						ft_onchannel_timeout,
						info, NULL);
	return false;
}

struct wiphy_radio_work_item_ops ft_onchannel_ops = {
	.do_work = ft_send_authenticate_onchannel,
};

int ft_authenticate_onchannel(uint32_t ifindex, const struct scan_bss *target)
{
	struct netdev *netdev = netdev_find(ifindex);
	struct handshake_state *hs = netdev_get_handshake(netdev);
	struct ft_info *info = ft_info_new(hs, target);

	info->onchannel = true;

	wiphy_radio_work_insert(netdev_get_wiphy(netdev), &info->work,
				WIPHY_WORK_PRIORITY_FT, &ft_onchannel_ops);
	l_queue_push_tail(info_list, info);

	return 0;
}

int ft_handshake_setup(uint32_t ifindex, const uint8_t *target)
{
	struct netdev *netdev = netdev_find(ifindex);
	struct handshake_state *hs = netdev_get_handshake(netdev);
	struct ft_info *info;
	int ret = 0;

	info = ft_info_find(ifindex, target);
	if (!info)
		return -ENOENT;

	/*
	 * Either failed or no response. This may have been an FT-over-DS
	 * attempt so clear out the entry so FT-over-Air can try again.
	 */
	if (info->status != 0) {
		int status = info->status;

		l_queue_remove(info_list, info);
		ft_info_destroy(info);

		return status;
	}

	/*
	 * This shouldn't ever fail:
	 *  - supplicant_ie has already been validated long ago
	 *  - l_checksum_* shouldn't fail since we presumable have kernel
	 *    support, how else could we have made it this far.
	 * But just in case...
	 */
	if (L_WARN_ON(!ft_prepare_handshake(info, hs)))
		ret = -EINVAL;

	/* After this no previous auths will be valid */
	ft_clear_authentications(ifindex);

	return ret;
}

static bool remove_ifindex(void *data, void *user_data)
{
	struct ft_info *info = data;
	uint32_t ifindex = L_PTR_TO_UINT(user_data);

	if (info->ifindex != ifindex)
		return false;

	if (info->offchannel_id)
		offchannel_cancel(netdev_get_wdev_id(netdev_find(ifindex)),
					info->offchannel_id);

	ft_info_destroy(info);
	return true;
}

void ft_clear_authentications(uint32_t ifindex)
{
	l_queue_foreach_remove(info_list, remove_ifindex,
				L_UINT_TO_PTR(ifindex));
}

static int ft_init(void)
{
	info_list = l_queue_new();

	return 0;
}

static void ft_exit(void)
{
	if (!l_queue_isempty(info_list))
		l_warn("stale FT info objects found!");

	l_queue_destroy(info_list, ft_info_destroy);
}

IWD_MODULE(ft, ft_init, ft_exit);
