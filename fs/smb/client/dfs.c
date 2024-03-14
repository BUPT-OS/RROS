// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022 Paulo Alcantara <palcantara@suse.de>
 */

#include "cifsproto.h"
#include "cifs_debug.h"
#include "dns_resolve.h"
#include "fs_context.h"
#include "dfs.h"

/**
 * dfs_parse_target_referral - set fs context for dfs target referral
 *
 * @full_path: full path in UNC format.
 * @ref: dfs referral pointer.
 * @ctx: smb3 fs context pointer.
 *
 * Return zero if dfs referral was parsed correctly, otherwise non-zero.
 */
int dfs_parse_target_referral(const char *full_path, const struct dfs_info3_param *ref,
			      struct smb3_fs_context *ctx)
{
	int rc;
	const char *prepath = NULL;
	char *path;

	if (!full_path || !*full_path || !ref || !ctx)
		return -EINVAL;

	if (WARN_ON_ONCE(!ref->node_name || ref->path_consumed < 0))
		return -EINVAL;

	if (strlen(full_path) - ref->path_consumed) {
		prepath = full_path + ref->path_consumed;
		/* skip initial delimiter */
		if (*prepath == '/' || *prepath == '\\')
			prepath++;
	}

	path = cifs_build_devname(ref->node_name, prepath);
	if (IS_ERR(path))
		return PTR_ERR(path);

	rc = smb3_parse_devname(path, ctx);
	if (rc)
		goto out;

	rc = dns_resolve_server_name_to_ip(path, (struct sockaddr *)&ctx->dstaddr, NULL);

out:
	kfree(path);
	return rc;
}

static int get_session(struct cifs_mount_ctx *mnt_ctx, const char *full_path)
{
	struct smb3_fs_context *ctx = mnt_ctx->fs_ctx;
	int rc;

	ctx->leaf_fullpath = (char *)full_path;
	rc = cifs_mount_get_session(mnt_ctx);
	ctx->leaf_fullpath = NULL;

	return rc;
}

/*
 * Track individual DFS referral servers used by new DFS mount.
 *
 * On success, their lifetime will be shared by final tcon (dfs_ses_list).
 * Otherwise, they will be put by dfs_put_root_smb_sessions() in cifs_mount().
 */
static int add_root_smb_session(struct cifs_mount_ctx *mnt_ctx)
{
	struct smb3_fs_context *ctx = mnt_ctx->fs_ctx;
	struct dfs_root_ses *root_ses;
	struct cifs_ses *ses = mnt_ctx->ses;

	if (ses) {
		root_ses = kmalloc(sizeof(*root_ses), GFP_KERNEL);
		if (!root_ses)
			return -ENOMEM;

		INIT_LIST_HEAD(&root_ses->list);

		spin_lock(&cifs_tcp_ses_lock);
		cifs_smb_ses_inc_refcount(ses);
		spin_unlock(&cifs_tcp_ses_lock);
		root_ses->ses = ses;
		list_add_tail(&root_ses->list, &mnt_ctx->dfs_ses_list);
	}
	/* Select new DFS referral server so that new referrals go through it */
	ctx->dfs_root_ses = ses;
	return 0;
}

static inline int parse_dfs_target(struct smb3_fs_context *ctx,
				   struct dfs_ref_walk *rw,
				   struct dfs_info3_param *tgt)
{
	int rc;
	const char *fpath = ref_walk_fpath(rw) + 1;

	rc = ref_walk_get_tgt(rw, tgt);
	if (!rc)
		rc = dfs_parse_target_referral(fpath, tgt, ctx);
	return rc;
}

static int set_ref_paths(struct cifs_mount_ctx *mnt_ctx,
			 struct dfs_info3_param *tgt,
			 struct dfs_ref_walk *rw)
{
	struct smb3_fs_context *ctx = mnt_ctx->fs_ctx;
	struct cifs_sb_info *cifs_sb = mnt_ctx->cifs_sb;
	char *ref_path, *full_path;
	int rc;

	full_path = smb3_fs_context_fullpath(ctx, CIFS_DIR_SEP(cifs_sb));
	if (IS_ERR(full_path))
		return PTR_ERR(full_path);

	if (!tgt || (tgt->server_type == DFS_TYPE_LINK &&
		     DFS_INTERLINK(tgt->flags)))
		ref_path = dfs_get_path(cifs_sb, ctx->UNC);
	else
		ref_path = dfs_get_path(cifs_sb, full_path);
	if (IS_ERR(ref_path)) {
		rc = PTR_ERR(ref_path);
		kfree(full_path);
		return rc;
	}
	ref_walk_path(rw) = ref_path;
	ref_walk_fpath(rw) = full_path;
	return 0;
}

static int __dfs_referral_walk(struct cifs_mount_ctx *mnt_ctx,
			       struct dfs_ref_walk *rw)
{
	struct smb3_fs_context *ctx = mnt_ctx->fs_ctx;
	struct dfs_info3_param tgt = {};
	bool is_refsrv;
	int rc = -ENOENT;

again:
	do {
		if (ref_walk_empty(rw)) {
			rc = dfs_get_referral(mnt_ctx, ref_walk_path(rw) + 1,
					      NULL, ref_walk_tl(rw));
			if (rc) {
				rc = cifs_mount_get_tcon(mnt_ctx);
				if (!rc)
					rc = cifs_is_path_remote(mnt_ctx);
				continue;
			}
			if (!ref_walk_num_tgts(rw)) {
				rc = -ENOENT;
				continue;
			}
		}

		while (ref_walk_next_tgt(rw)) {
			rc = parse_dfs_target(ctx, rw, &tgt);
			if (rc)
				continue;

			cifs_mount_put_conns(mnt_ctx);
			rc = get_session(mnt_ctx, ref_walk_path(rw));
			if (rc)
				continue;

			is_refsrv = tgt.server_type == DFS_TYPE_ROOT ||
				DFS_INTERLINK(tgt.flags);
			ref_walk_set_tgt_hint(rw);

			if (tgt.flags & DFSREF_STORAGE_SERVER) {
				rc = cifs_mount_get_tcon(mnt_ctx);
				if (!rc)
					rc = cifs_is_path_remote(mnt_ctx);
				if (!rc)
					break;
				if (rc != -EREMOTE)
					continue;
			}

			if (is_refsrv) {
				rc = add_root_smb_session(mnt_ctx);
				if (rc)
					goto out;
			}

			rc = ref_walk_advance(rw);
			if (!rc) {
				rc = set_ref_paths(mnt_ctx, &tgt, rw);
				if (!rc) {
					rc = -EREMOTE;
					goto again;
				}
			}
			if (rc != -ELOOP)
				goto out;
		}
	} while (rc && ref_walk_descend(rw));

out:
	free_dfs_info_param(&tgt);
	return rc;
}

static int dfs_referral_walk(struct cifs_mount_ctx *mnt_ctx)
{
	struct dfs_ref_walk *rw;
	int rc;

	rw = ref_walk_alloc();
	if (IS_ERR(rw))
		return PTR_ERR(rw);

	ref_walk_init(rw);
	rc = set_ref_paths(mnt_ctx, NULL, rw);
	if (!rc)
		rc = __dfs_referral_walk(mnt_ctx, rw);
	ref_walk_free(rw);
	return rc;
}

static int __dfs_mount_share(struct cifs_mount_ctx *mnt_ctx)
{
	struct cifs_sb_info *cifs_sb = mnt_ctx->cifs_sb;
	struct smb3_fs_context *ctx = mnt_ctx->fs_ctx;
	struct cifs_tcon *tcon;
	char *origin_fullpath;
	int rc;

	origin_fullpath = dfs_get_path(cifs_sb, ctx->source);
	if (IS_ERR(origin_fullpath))
		return PTR_ERR(origin_fullpath);

	rc = dfs_referral_walk(mnt_ctx);
	if (rc)
		goto out;

	tcon = mnt_ctx->tcon;
	spin_lock(&tcon->tc_lock);
	if (!tcon->origin_fullpath) {
		tcon->origin_fullpath = origin_fullpath;
		origin_fullpath = NULL;
	}
	spin_unlock(&tcon->tc_lock);

	if (list_empty(&tcon->dfs_ses_list)) {
		list_replace_init(&mnt_ctx->dfs_ses_list, &tcon->dfs_ses_list);
		queue_delayed_work(dfscache_wq, &tcon->dfs_cache_work,
				   dfs_cache_get_ttl() * HZ);
	} else {
		dfs_put_root_smb_sessions(&mnt_ctx->dfs_ses_list);
	}

out:
	kfree(origin_fullpath);
	return rc;
}

/* Resolve UNC hostname in @ctx->source and set ip addr in @ctx->dstaddr */
static int update_fs_context_dstaddr(struct smb3_fs_context *ctx)
{
	struct sockaddr *addr = (struct sockaddr *)&ctx->dstaddr;
	int rc;

	rc = dns_resolve_server_name_to_ip(ctx->source, addr, NULL);
	if (!rc)
		cifs_set_port(addr, ctx->port);
	return rc;
}

int dfs_mount_share(struct cifs_mount_ctx *mnt_ctx, bool *isdfs)
{
	struct smb3_fs_context *ctx = mnt_ctx->fs_ctx;
	bool nodfs = ctx->nodfs;
	int rc;

	rc = update_fs_context_dstaddr(ctx);
	if (rc)
		return rc;

	*isdfs = false;
	rc = get_session(mnt_ctx, NULL);
	if (rc)
		return rc;

	ctx->dfs_root_ses = mnt_ctx->ses;
	/*
	 * If called with 'nodfs' mount option, then skip DFS resolving.  Otherwise unconditionally
	 * try to get an DFS referral (even cached) to determine whether it is an DFS mount.
	 *
	 * Skip prefix path to provide support for DFS referrals from w2k8 servers which don't seem
	 * to respond with PATH_NOT_COVERED to requests that include the prefix.
	 */
	if (!nodfs) {
		rc = dfs_get_referral(mnt_ctx, ctx->UNC + 1, NULL, NULL);
		if (rc) {
			cifs_dbg(FYI, "%s: no dfs referral for %s: %d\n",
				 __func__, ctx->UNC + 1, rc);
			cifs_dbg(FYI, "%s: assuming non-dfs mount...\n", __func__);
			nodfs = true;
		}
	}
	if (nodfs) {
		rc = cifs_mount_get_tcon(mnt_ctx);
		if (!rc)
			rc = cifs_is_path_remote(mnt_ctx);
		return rc;
	}

	*isdfs = true;
	add_root_smb_session(mnt_ctx);
	return __dfs_mount_share(mnt_ctx);
}

/* Update dfs referral path of superblock */
static int update_server_fullpath(struct TCP_Server_Info *server, struct cifs_sb_info *cifs_sb,
				  const char *target)
{
	int rc = 0;
	size_t len = strlen(target);
	char *refpath, *npath;

	if (unlikely(len < 2 || *target != '\\'))
		return -EINVAL;

	if (target[1] == '\\') {
		len += 1;
		refpath = kmalloc(len, GFP_KERNEL);
		if (!refpath)
			return -ENOMEM;

		scnprintf(refpath, len, "%s", target);
	} else {
		len += sizeof("\\");
		refpath = kmalloc(len, GFP_KERNEL);
		if (!refpath)
			return -ENOMEM;

		scnprintf(refpath, len, "\\%s", target);
	}

	npath = dfs_cache_canonical_path(refpath, cifs_sb->local_nls, cifs_remap(cifs_sb));
	kfree(refpath);

	if (IS_ERR(npath)) {
		rc = PTR_ERR(npath);
	} else {
		mutex_lock(&server->refpath_lock);
		spin_lock(&server->srv_lock);
		kfree(server->leaf_fullpath);
		server->leaf_fullpath = npath;
		spin_unlock(&server->srv_lock);
		mutex_unlock(&server->refpath_lock);
	}
	return rc;
}

static int target_share_matches_server(struct TCP_Server_Info *server, char *share,
				       bool *target_match)
{
	int rc = 0;
	const char *dfs_host;
	size_t dfs_host_len;

	*target_match = true;
	extract_unc_hostname(share, &dfs_host, &dfs_host_len);

	/* Check if hostnames or addresses match */
	cifs_server_lock(server);
	if (dfs_host_len != strlen(server->hostname) ||
	    strncasecmp(dfs_host, server->hostname, dfs_host_len)) {
		cifs_dbg(FYI, "%s: %.*s doesn't match %s\n", __func__,
			 (int)dfs_host_len, dfs_host, server->hostname);
		rc = match_target_ip(server, dfs_host, dfs_host_len, target_match);
		if (rc)
			cifs_dbg(VFS, "%s: failed to match target ip: %d\n", __func__, rc);
	}
	cifs_server_unlock(server);
	return rc;
}

static void __tree_connect_ipc(const unsigned int xid, char *tree,
			       struct cifs_sb_info *cifs_sb,
			       struct cifs_ses *ses)
{
	struct TCP_Server_Info *server = ses->server;
	struct cifs_tcon *tcon = ses->tcon_ipc;
	int rc;

	spin_lock(&ses->ses_lock);
	spin_lock(&ses->chan_lock);
	if (cifs_chan_needs_reconnect(ses, server) ||
	    ses->ses_status != SES_GOOD) {
		spin_unlock(&ses->chan_lock);
		spin_unlock(&ses->ses_lock);
		cifs_server_dbg(FYI, "%s: skipping ipc reconnect due to disconnected ses\n",
				__func__);
		return;
	}
	spin_unlock(&ses->chan_lock);
	spin_unlock(&ses->ses_lock);

	cifs_server_lock(server);
	scnprintf(tree, MAX_TREE_SIZE, "\\\\%s\\IPC$", server->hostname);
	cifs_server_unlock(server);

	rc = server->ops->tree_connect(xid, ses, tree, tcon,
				       cifs_sb->local_nls);
	cifs_server_dbg(FYI, "%s: tree_reconnect %s: %d\n", __func__, tree, rc);
	spin_lock(&tcon->tc_lock);
	if (rc) {
		tcon->status = TID_NEED_TCON;
	} else {
		tcon->status = TID_GOOD;
		tcon->need_reconnect = false;
	}
	spin_unlock(&tcon->tc_lock);
}

static void tree_connect_ipc(const unsigned int xid, char *tree,
			     struct cifs_sb_info *cifs_sb,
			     struct cifs_tcon *tcon)
{
	struct cifs_ses *ses = tcon->ses;

	__tree_connect_ipc(xid, tree, cifs_sb, ses);
	__tree_connect_ipc(xid, tree, cifs_sb, CIFS_DFS_ROOT_SES(ses));
}

static int __tree_connect_dfs_target(const unsigned int xid, struct cifs_tcon *tcon,
				     struct cifs_sb_info *cifs_sb, char *tree, bool islink,
				     struct dfs_cache_tgt_list *tl)
{
	int rc;
	struct TCP_Server_Info *server = tcon->ses->server;
	const struct smb_version_operations *ops = server->ops;
	struct cifs_ses *root_ses = CIFS_DFS_ROOT_SES(tcon->ses);
	char *share = NULL, *prefix = NULL;
	struct dfs_cache_tgt_iterator *tit;
	bool target_match;

	tit = dfs_cache_get_tgt_iterator(tl);
	if (!tit) {
		rc = -ENOENT;
		goto out;
	}

	/* Try to tree connect to all dfs targets */
	for (; tit; tit = dfs_cache_get_next_tgt(tl, tit)) {
		const char *target = dfs_cache_get_tgt_name(tit);
		DFS_CACHE_TGT_LIST(ntl);

		kfree(share);
		kfree(prefix);
		share = prefix = NULL;

		/* Check if share matches with tcp ses */
		rc = dfs_cache_get_tgt_share(server->leaf_fullpath + 1, tit, &share, &prefix);
		if (rc) {
			cifs_dbg(VFS, "%s: failed to parse target share: %d\n", __func__, rc);
			break;
		}

		rc = target_share_matches_server(server, share, &target_match);
		if (rc)
			break;
		if (!target_match) {
			rc = -EHOSTUNREACH;
			continue;
		}

		dfs_cache_noreq_update_tgthint(server->leaf_fullpath + 1, tit);
		tree_connect_ipc(xid, tree, cifs_sb, tcon);

		scnprintf(tree, MAX_TREE_SIZE, "\\%s", share);
		if (!islink) {
			rc = ops->tree_connect(xid, tcon->ses, tree, tcon, cifs_sb->local_nls);
			break;
		}

		/*
		 * If no dfs referrals were returned from link target, then just do a TREE_CONNECT
		 * to it.  Otherwise, cache the dfs referral and then mark current tcp ses for
		 * reconnect so either the demultiplex thread or the echo worker will reconnect to
		 * newly resolved target.
		 */
		if (dfs_cache_find(xid, root_ses, cifs_sb->local_nls, cifs_remap(cifs_sb), target,
				   NULL, &ntl)) {
			rc = ops->tree_connect(xid, tcon->ses, tree, tcon, cifs_sb->local_nls);
			if (rc)
				continue;

			rc = cifs_update_super_prepath(cifs_sb, prefix);
		} else {
			/* Target is another dfs share */
			rc = update_server_fullpath(server, cifs_sb, target);
			dfs_cache_free_tgts(tl);

			if (!rc) {
				rc = -EREMOTE;
				list_replace_init(&ntl.tl_list, &tl->tl_list);
			} else
				dfs_cache_free_tgts(&ntl);
		}
		break;
	}

out:
	kfree(share);
	kfree(prefix);

	return rc;
}

static int tree_connect_dfs_target(const unsigned int xid, struct cifs_tcon *tcon,
				   struct cifs_sb_info *cifs_sb, char *tree, bool islink,
				   struct dfs_cache_tgt_list *tl)
{
	int rc;
	int num_links = 0;
	struct TCP_Server_Info *server = tcon->ses->server;
	char *old_fullpath = server->leaf_fullpath;

	do {
		rc = __tree_connect_dfs_target(xid, tcon, cifs_sb, tree, islink, tl);
		if (!rc || rc != -EREMOTE)
			break;
	} while (rc = -ELOOP, ++num_links < MAX_NESTED_LINKS);
	/*
	 * If we couldn't tree connect to any targets from last referral path, then
	 * retry it from newly resolved dfs referral.
	 */
	if (rc && server->leaf_fullpath != old_fullpath)
		cifs_signal_cifsd_for_reconnect(server, true);

	dfs_cache_free_tgts(tl);
	return rc;
}

int cifs_tree_connect(const unsigned int xid, struct cifs_tcon *tcon, const struct nls_table *nlsc)
{
	int rc;
	struct TCP_Server_Info *server = tcon->ses->server;
	const struct smb_version_operations *ops = server->ops;
	DFS_CACHE_TGT_LIST(tl);
	struct cifs_sb_info *cifs_sb = NULL;
	struct super_block *sb = NULL;
	struct dfs_info3_param ref = {0};
	char *tree;

	/* only send once per connect */
	spin_lock(&tcon->tc_lock);
	if (tcon->status == TID_GOOD) {
		spin_unlock(&tcon->tc_lock);
		return 0;
	}

	if (tcon->status != TID_NEW &&
	    tcon->status != TID_NEED_TCON) {
		spin_unlock(&tcon->tc_lock);
		return -EHOSTDOWN;
	}

	tcon->status = TID_IN_TCON;
	spin_unlock(&tcon->tc_lock);

	tree = kzalloc(MAX_TREE_SIZE, GFP_KERNEL);
	if (!tree) {
		rc = -ENOMEM;
		goto out;
	}

	if (tcon->ipc) {
		cifs_server_lock(server);
		scnprintf(tree, MAX_TREE_SIZE, "\\\\%s\\IPC$", server->hostname);
		cifs_server_unlock(server);
		rc = ops->tree_connect(xid, tcon->ses, tree, tcon, nlsc);
		goto out;
	}

	sb = cifs_get_dfs_tcon_super(tcon);
	if (!IS_ERR(sb))
		cifs_sb = CIFS_SB(sb);

	/*
	 * Tree connect to last share in @tcon->tree_name whether dfs super or
	 * cached dfs referral was not found.
	 */
	if (!cifs_sb || !server->leaf_fullpath ||
	    dfs_cache_noreq_find(server->leaf_fullpath + 1, &ref, &tl)) {
		rc = ops->tree_connect(xid, tcon->ses, tcon->tree_name, tcon,
				       cifs_sb ? cifs_sb->local_nls : nlsc);
		goto out;
	}

	rc = tree_connect_dfs_target(xid, tcon, cifs_sb, tree, ref.server_type == DFS_TYPE_LINK,
				     &tl);
	free_dfs_info_param(&ref);

out:
	kfree(tree);
	cifs_put_tcp_super(sb);

	if (rc) {
		spin_lock(&tcon->tc_lock);
		if (tcon->status == TID_IN_TCON)
			tcon->status = TID_NEED_TCON;
		spin_unlock(&tcon->tc_lock);
	} else {
		spin_lock(&tcon->tc_lock);
		if (tcon->status == TID_IN_TCON)
			tcon->status = TID_GOOD;
		spin_unlock(&tcon->tc_lock);
		tcon->need_reconnect = false;
	}

	return rc;
}
