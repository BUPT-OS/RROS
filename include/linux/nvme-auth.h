/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2021 Hannes Reinecke, SUSE Software Solutions
 */

#ifndef _NVME_AUTH_H
#define _NVME_AUTH_H

#include <crypto/kpp.h>

struct nvme_dhchap_key {
	u8 *key;
	size_t len;
	u8 hash;
};

u32 nvme_auth_get_seqnum(void);
const char *nvme_auth_dhgroup_name(u8 dhgroup_id);
const char *nvme_auth_dhgroup_kpp(u8 dhgroup_id);
u8 nvme_auth_dhgroup_id(const char *dhgroup_name);

const char *nvme_auth_hmac_name(u8 hmac_id);
const char *nvme_auth_digest_name(u8 hmac_id);
size_t nvme_auth_hmac_hash_len(u8 hmac_id);
u8 nvme_auth_hmac_id(const char *hmac_name);

struct nvme_dhchap_key *nvme_auth_extract_key(unsigned char *secret,
					      u8 key_hash);
void nvme_auth_free_key(struct nvme_dhchap_key *key);
u8 *nvme_auth_transform_key(struct nvme_dhchap_key *key, char *nqn);
int nvme_auth_generate_key(u8 *secret, struct nvme_dhchap_key **ret_key);
int nvme_auth_augmented_challenge(u8 hmac_id, u8 *skey, size_t skey_len,
				  u8 *challenge, u8 *aug, size_t hlen);
int nvme_auth_gen_privkey(struct crypto_kpp *dh_tfm, u8 dh_gid);
int nvme_auth_gen_pubkey(struct crypto_kpp *dh_tfm,
			 u8 *host_key, size_t host_key_len);
int nvme_auth_gen_shared_secret(struct crypto_kpp *dh_tfm,
				u8 *ctrl_key, size_t ctrl_key_len,
				u8 *sess_key, size_t sess_key_len);

#endif /* _NVME_AUTH_H */
