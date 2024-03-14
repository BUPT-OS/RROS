/* SPDX-License-Identifier: GPL-2.0-only */
/******************************************************************************
*******************************************************************************
**
**  Copyright (C) Sistina Software, Inc.  1997-2003  All rights reserved.
**  Copyright (C) 2004-2005 Red Hat, Inc.  All rights reserved.
**
**
*******************************************************************************
******************************************************************************/

#ifndef __MIDCOMMS_DOT_H__
#define __MIDCOMMS_DOT_H__

struct midcomms_node;

int dlm_validate_incoming_buffer(int nodeid, unsigned char *buf, int len);
int dlm_process_incoming_buffer(int nodeid, unsigned char *buf, int buflen);
struct dlm_mhandle *dlm_midcomms_get_mhandle(int nodeid, int len,
					     gfp_t allocation, char **ppc);
void dlm_midcomms_commit_mhandle(struct dlm_mhandle *mh, const void *name,
				 int namelen);
int dlm_midcomms_addr(int nodeid, struct sockaddr_storage *addr, int len);
void dlm_midcomms_version_wait(void);
int dlm_midcomms_close(int nodeid);
int dlm_midcomms_start(void);
void dlm_midcomms_stop(void);
void dlm_midcomms_init(void);
void dlm_midcomms_exit(void);
void dlm_midcomms_shutdown(void);
void dlm_midcomms_add_member(int nodeid);
void dlm_midcomms_remove_member(int nodeid);
void dlm_midcomms_unack_msg_resend(int nodeid);
const char *dlm_midcomms_state(struct midcomms_node *node);
unsigned long dlm_midcomms_flags(struct midcomms_node *node);
int dlm_midcomms_send_queue_cnt(struct midcomms_node *node);
uint32_t dlm_midcomms_version(struct midcomms_node *node);
int dlm_midcomms_rawmsg_send(struct midcomms_node *node, void *buf,
			     int buflen);
struct kmem_cache *dlm_midcomms_cache_create(void);

#endif				/* __MIDCOMMS_DOT_H__ */

