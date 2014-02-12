/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License version 2 for more details.  A copy is
 * included in the COPYING file that accompanied this code.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2012, 2013, Intel Corporation.
 */
/*
 * lustre/lfsck/lfsck_bookmark.c
 *
 * Author: Fan, Yong <fan.yong@intel.com>
 */

#define DEBUG_SUBSYSTEM S_LFSCK

#include <lu_object.h>
#include <dt_object.h>
#include <lustre_fid.h>
#include <lustre/lustre_user.h>

#include "lfsck_internal.h"

#define LFSCK_BOOKMARK_MAGIC	0x20130C1D

static const char lfsck_bookmark_name[] = "lfsck_bookmark";

static void lfsck_bookmark_le_to_cpu(struct lfsck_bookmark *des,
				     struct lfsck_bookmark *src)
{
	des->lb_magic = le32_to_cpu(src->lb_magic);
	des->lb_version = le16_to_cpu(src->lb_version);
	des->lb_param = le16_to_cpu(src->lb_param);
	des->lb_speed_limit = le32_to_cpu(src->lb_speed_limit);
	des->lb_async_windows = le16_to_cpu(src->lb_async_windows);
	fid_le_to_cpu(&des->lb_lpf_fid, &src->lb_lpf_fid);
	fid_le_to_cpu(&des->lb_last_fid, &src->lb_last_fid);
}

void lfsck_bookmark_cpu_to_le(struct lfsck_bookmark *des,
			      struct lfsck_bookmark *src)
{
	des->lb_magic = cpu_to_le32(src->lb_magic);
	des->lb_version = cpu_to_le16(src->lb_version);
	des->lb_param = cpu_to_le16(src->lb_param);
	des->lb_speed_limit = cpu_to_le32(src->lb_speed_limit);
	des->lb_async_windows = cpu_to_le16(src->lb_async_windows);
	fid_cpu_to_le(&des->lb_lpf_fid, &src->lb_lpf_fid);
	fid_cpu_to_le(&des->lb_last_fid, &src->lb_last_fid);
}

static int lfsck_bookmark_load(const struct lu_env *env,
			       struct lfsck_instance *lfsck)
{
	loff_t pos = 0;
	int    len = sizeof(struct lfsck_bookmark);
	int    rc;

	rc = dt_record_read(env, lfsck->li_bookmark_obj,
			    lfsck_buf_get(env, &lfsck->li_bookmark_disk, len),
			    &pos);
	if (rc == 0) {
		struct lfsck_bookmark *bm = &lfsck->li_bookmark_ram;

		lfsck_bookmark_le_to_cpu(bm, &lfsck->li_bookmark_disk);
		if (bm->lb_magic != LFSCK_BOOKMARK_MAGIC) {
			CWARN("%s: invalid lfsck_bookmark magic %#x != %#x\n",
			      lfsck_lfsck2name(lfsck), bm->lb_magic,
			      LFSCK_BOOKMARK_MAGIC);
			/* Process it as new lfsck_bookmark. */
			rc = -ENODATA;
		}
	} else {
		if (rc == -EFAULT && pos == 0)
			/* return -ENODATA for empty lfsck_bookmark. */
			rc = -ENODATA;
		else
			CERROR("%s: fail to load lfsck_bookmark: "
			       "expected = %d, rc = %d\n",
			       lfsck_lfsck2name(lfsck), len, rc);
	}
	return rc;
}

int lfsck_bookmark_store(const struct lu_env *env, struct lfsck_instance *lfsck)
{
	struct thandle    *handle;
	struct dt_object  *obj    = lfsck->li_bookmark_obj;
	loff_t		   pos    = 0;
	int		   len    = sizeof(struct lfsck_bookmark);
	int		   rc;
	ENTRY;

	lfsck_bookmark_cpu_to_le(&lfsck->li_bookmark_disk,
				 &lfsck->li_bookmark_ram);
	handle = dt_trans_create(env, lfsck->li_bottom);
	if (IS_ERR(handle)) {
		rc = PTR_ERR(handle);
		CERROR("%s: fail to create trans for storing lfsck_bookmark: "
		       "rc = %d\n", lfsck_lfsck2name(lfsck), rc);
		RETURN(rc);
	}

	rc = dt_declare_record_write(env, obj, len, 0, handle);
	if (rc != 0) {
		CERROR("%s: fail to declare trans for storing lfsck_bookmark: "
		       "rc = %d\n", lfsck_lfsck2name(lfsck), rc);
		GOTO(out, rc);
	}

	rc = dt_trans_start_local(env, lfsck->li_bottom, handle);
	if (rc != 0) {
		CERROR("%s: fail to start trans for storing lfsck_bookmark: "
		       "rc = %d\n", lfsck_lfsck2name(lfsck), rc);
		GOTO(out, rc);
	}

	rc = dt_record_write(env, obj,
			     lfsck_buf_get(env, &lfsck->li_bookmark_disk, len),
			     &pos, handle);
	if (rc != 0)
		CERROR("%s: fail to store lfsck_bookmark: expected = %d, "
		       "rc = %d\n", lfsck_lfsck2name(lfsck), len, rc);

	GOTO(out, rc);

out:
	dt_trans_stop(env, lfsck->li_bottom, handle);
	return rc;
}

static int lfsck_bookmark_init(const struct lu_env *env,
			       struct lfsck_instance *lfsck)
{
	struct lfsck_bookmark *mb = &lfsck->li_bookmark_ram;
	int rc;

	memset(mb, 0, sizeof(*mb));
	mb->lb_magic = LFSCK_BOOKMARK_MAGIC;
	mb->lb_version = LFSCK_VERSION_V2;
	mb->lb_async_windows = LFSCK_ASYNC_WIN_DEFAULT;
	mutex_lock(&lfsck->li_mutex);
	rc = lfsck_bookmark_store(env, lfsck);
	mutex_unlock(&lfsck->li_mutex);
	return rc;
}

int lfsck_bookmark_setup(const struct lu_env *env,
			 struct lfsck_instance *lfsck)
{
	struct dt_object *root;
	struct dt_object *obj;
	int		  rc;
	ENTRY;

	root = dt_locate(env, lfsck->li_bottom, &lfsck->li_local_root_fid);
	if (IS_ERR(root))
		RETURN(PTR_ERR(root));

	if (unlikely(!dt_try_as_dir(env, root))) {
		lu_object_put(env, &root->do_lu);

		RETURN(-ENOTDIR);
	}

	obj = local_file_find_or_create(env, lfsck->li_los, root,
					lfsck_bookmark_name,
					S_IFREG | S_IRUGO | S_IWUSR);
	lu_object_put(env, &root->do_lu);
	if (IS_ERR(obj))
		RETURN(PTR_ERR(obj));

	lfsck->li_bookmark_obj = obj;
	rc = lfsck_bookmark_load(env, lfsck);
	if (rc == -ENODATA)
		rc = lfsck_bookmark_init(env, lfsck);

	RETURN(rc);
}
