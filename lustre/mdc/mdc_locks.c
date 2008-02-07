/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2001-2003 Cluster File Systems, Inc.
 *
 *   This file is part of the Lustre file system, http://www.lustre.org
 *   Lustre is a trademark of Cluster File Systems, Inc.
 *
 *   You may have signed or agreed to another license before downloading
 *   this software.  If so, you are bound by the terms and conditions
 *   of that agreement, and the following does not apply to you.  See the
 *   LICENSE file included with this distribution for more information.
 *
 *   If you did not agree to a different license, then this copy of Lustre
 *   is open source software; you can redistribute it and/or modify it
 *   under the terms of version 2 of the GNU General Public License as
 *   published by the Free Software Foundation.
 *
 *   In either case, Lustre is distributed in the hope that it will be
 *   useful, but WITHOUT ANY WARRANTY; without even the implied warranty
 *   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   license text for more details.
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_MDC

#ifdef __KERNEL__
# include <linux/module.h>
# include <linux/pagemap.h>
# include <linux/miscdevice.h>
# include <linux/init.h>
#else
# include <liblustre.h>
#endif

#include <linux/lustre_acl.h>
#include <obd_class.h>
#include <lustre_dlm.h>
/* fid_res_name_eq() */
#include <lustre_fid.h>
#include <lprocfs_status.h>
#include "mdc_internal.h"

int it_disposition(struct lookup_intent *it, int flag)
{
        return it->d.lustre.it_disposition & flag;
}
EXPORT_SYMBOL(it_disposition);

void it_set_disposition(struct lookup_intent *it, int flag)
{
        it->d.lustre.it_disposition |= flag;
}
EXPORT_SYMBOL(it_set_disposition);

void it_clear_disposition(struct lookup_intent *it, int flag)
{
        it->d.lustre.it_disposition &= ~flag;
}
EXPORT_SYMBOL(it_clear_disposition);

static int it_to_lock_mode(struct lookup_intent *it)
{
        ENTRY;

        /* CREAT needs to be tested before open (both could be set) */
        if (it->it_op & IT_CREAT)
                return LCK_PW;
        else if (it->it_op & (IT_READDIR | IT_GETATTR | IT_OPEN | IT_LOOKUP))
                return LCK_PR;

        LBUG();
        RETURN(-EINVAL);
}

int it_open_error(int phase, struct lookup_intent *it)
{
        if (it_disposition(it, DISP_OPEN_OPEN)) {
                if (phase >= DISP_OPEN_OPEN)
                        return it->d.lustre.it_status;
                else
                        return 0;
        }

        if (it_disposition(it, DISP_OPEN_CREATE)) {
                if (phase >= DISP_OPEN_CREATE)
                        return it->d.lustre.it_status;
                else
                        return 0;
        }

        if (it_disposition(it, DISP_LOOKUP_EXECD)) {
                if (phase >= DISP_LOOKUP_EXECD)
                        return it->d.lustre.it_status;
                else
                        return 0;
        }

        if (it_disposition(it, DISP_IT_EXECD)) {
                if (phase >= DISP_IT_EXECD)
                        return it->d.lustre.it_status;
                else
                        return 0;
        }
        CERROR("it disp: %X, status: %d\n", it->d.lustre.it_disposition,
               it->d.lustre.it_status);
        LBUG();
        return 0;
}
EXPORT_SYMBOL(it_open_error);

/* this must be called on a lockh that is known to have a referenced lock */
int mdc_set_lock_data(struct obd_export *exp, __u64 *lockh, void *data)
{
        struct ldlm_lock *lock;
        ENTRY;

        if (!*lockh) {
                EXIT;
                RETURN(0);
        }

        lock = ldlm_handle2lock((struct lustre_handle *)lockh);

        LASSERT(lock != NULL);
        lock_res_and_lock(lock);
#ifdef __KERNEL__
        if (lock->l_ast_data && lock->l_ast_data != data) {
                struct inode *new_inode = data;
                struct inode *old_inode = lock->l_ast_data;
                LASSERTF(old_inode->i_state & I_FREEING,
                         "Found existing inode %p/%lu/%u state %lu in lock: "
                         "setting data to %p/%lu/%u\n", old_inode,
                         old_inode->i_ino, old_inode->i_generation,
                         old_inode->i_state,
                         new_inode, new_inode->i_ino, new_inode->i_generation);
        }
#endif
        lock->l_ast_data = data;
        unlock_res_and_lock(lock);
        LDLM_LOCK_PUT(lock);

        RETURN(0);
}

ldlm_mode_t mdc_lock_match(struct obd_export *exp, int flags,
                           const struct lu_fid *fid, ldlm_type_t type,
                           ldlm_policy_data_t *policy, ldlm_mode_t mode,
                           struct lustre_handle *lockh)
{
        struct ldlm_res_id res_id =
                { .name = {fid_seq(fid),
                           fid_oid(fid),
                           fid_ver(fid)} };
        ldlm_mode_t rc;
        ENTRY;

        rc = ldlm_lock_match(class_exp2obd(exp)->obd_namespace, flags,
                             &res_id, type, policy, mode, lockh);
        RETURN(rc);
}

int mdc_cancel_unused(struct obd_export *exp,
                      const struct lu_fid *fid,
                      ldlm_policy_data_t *policy,
                      ldlm_mode_t mode, int flags, void *opaque)
{
        struct ldlm_res_id res_id =
                { .name = {fid_seq(fid),
                           fid_oid(fid),
                           fid_ver(fid)} };
        struct obd_device *obd = class_exp2obd(exp);
        int rc;

        ENTRY;

        rc = ldlm_cli_cancel_unused_resource(obd->obd_namespace, &res_id,
                                             policy, mode, flags, opaque);
        RETURN(rc);
}

int mdc_change_cbdata(struct obd_export *exp,
                      const struct lu_fid *fid,
                      ldlm_iterator_t it, void *data)
{
        struct ldlm_res_id res_id = { .name = {0} };
        ENTRY;

        res_id.name[0] = fid_seq(fid);
        res_id.name[1] = fid_oid(fid);
        res_id.name[2] = fid_ver(fid);

        ldlm_resource_iterate(class_exp2obd(exp)->obd_namespace,
                              &res_id, it, data);

        EXIT;
        return 0;
}

static inline void mdc_clear_replay_flag(struct ptlrpc_request *req, int rc)
{
        /* Don't hold error requests for replay. */
        if (req->rq_replay) {
                spin_lock(&req->rq_lock);
                req->rq_replay = 0;
                spin_unlock(&req->rq_lock);
        }
        if (rc && req->rq_transno != 0) {
                DEBUG_REQ(D_ERROR, req, "transno returned on error rc %d", rc);
                LBUG();
        }
}

/* Save a large LOV EA into the request buffer so that it is available
 * for replay.  We don't do this in the initial request because the
 * original request doesn't need this buffer (at most it sends just the
 * lov_mds_md) and it is a waste of RAM/bandwidth to send the empty
 * buffer and may also be difficult to allocate and save a very large
 * request buffer for each open. (bug 5707)
 *
 * OOM here may cause recovery failure if lmm is needed (only for the
 * original open if the MDS crashed just when this client also OOM'd)
 * but this is incredibly unlikely, and questionable whether the client
 * could do MDS recovery under OOM anyways... */
static void mdc_realloc_openmsg(struct ptlrpc_request *req,
                               struct mdt_body *body)
{
        int     rc;

        /* FIXME: remove this explicit offset. */
        rc = sptlrpc_cli_enlarge_reqbuf(req, DLM_INTENT_REC_OFF + 4,
                                        body->eadatasize);
        if (rc) {
                CERROR("Can't enlarge segment %d size to %d\n",
                       DLM_INTENT_REC_OFF + 4, body->eadatasize);
                body->valid &= ~OBD_MD_FLEASIZE;
                body->eadatasize = 0;
        }
}

static struct ptlrpc_request *mdc_intent_open_pack(struct obd_export *exp,
                                                   struct lookup_intent *it,
                                                   struct md_op_data *op_data,
                                                   void *lmm, int lmmsize,
                                                   void *cb_data)
{
        struct ptlrpc_request *req;
        struct obd_device     *obddev = class_exp2obd(exp);
        struct ldlm_intent    *lit;
        int                    joinfile = !!((it->it_flags & O_JOIN_FILE) && 
                                              op_data->op_data);
        CFS_LIST_HEAD(cancels);
        int                    count = 0;
        int                    mode;
        int                    rc;
        ENTRY;

        it->it_create_mode = (it->it_create_mode & ~S_IFMT) | S_IFREG;

        /* XXX: openlock is not cancelled for cross-refs. */
        /* If inode is known, cancel conflicting OPEN locks. */
        if (fid_is_sane(&op_data->op_fid2)) {
                if (it->it_flags & (FMODE_WRITE|MDS_OPEN_TRUNC))
                        mode = LCK_CW;
#ifdef FMODE_EXEC
                else if (it->it_flags & FMODE_EXEC)
                        mode = LCK_PR;
#endif
                else
                        mode = LCK_CR;
                count = mdc_resource_get_unused(exp, &op_data->op_fid2,
                                                &cancels, mode,
                                                MDS_INODELOCK_OPEN);
        }

        /* If CREATE or JOIN_FILE, cancel parent's UPDATE lock. */
        if (it->it_op & IT_CREAT || joinfile)
                mode = LCK_EX;
        else
                mode = LCK_CR;
        count += mdc_resource_get_unused(exp, &op_data->op_fid1,
                                         &cancels, mode,
                                         MDS_INODELOCK_UPDATE);

        req = ptlrpc_request_alloc(class_exp2cliimp(exp),
                                   &RQF_LDLM_INTENT_OPEN);
        if (req == NULL) {
                ldlm_lock_list_put(&cancels, l_bl_ast, count);
                RETURN(ERR_PTR(-ENOMEM));
        }

        /* parent capability */
        mdc_set_capa_size(req, &RMF_CAPA1, op_data->op_capa1);
        /* child capability, reserve the size according to parent capa, it will
         * be filled after we get the reply */
        mdc_set_capa_size(req, &RMF_CAPA2, op_data->op_capa1);

        req_capsule_set_size(&req->rq_pill, &RMF_NAME, RCL_CLIENT,
                             op_data->op_namelen + 1);
        req_capsule_set_size(&req->rq_pill, &RMF_EADATA, RCL_CLIENT,
                             max(lmmsize, obddev->u.cli.cl_default_mds_easize));
        if (!joinfile) {
                req_capsule_set_size(&req->rq_pill, &RMF_REC_JOINFILE,
                                     RCL_CLIENT, 0);
        }

        if (exp_connect_cancelset(exp) && count) {
                req_capsule_set_size(&req->rq_pill, &RMF_DLM_REQ, RCL_CLIENT,
                                     ldlm_request_bufsize(count, LDLM_ENQUEUE));
        }

        rc = ptlrpc_request_pack(req, LUSTRE_DLM_VERSION, LDLM_ENQUEUE);
        if (rc) {
                ptlrpc_request_free(req);
                ldlm_lock_list_put(&cancels, l_bl_ast, count);
                return NULL;
        }
        if (exp_connect_cancelset(exp) && req)
                ldlm_cli_cancel_list(&cancels, count, req, 0);

        if (joinfile) {
                __u64 head_size = *(__u64 *)op_data->op_data;
                mdc_join_pack(req, op_data, head_size);
        }

        spin_lock(&req->rq_lock);
        req->rq_replay = 1;
        spin_unlock(&req->rq_lock);

        /* pack the intent */
        lit = req_capsule_client_get(&req->rq_pill, &RMF_LDLM_INTENT);
        lit->opc = (__u64)it->it_op;

        /* pack the intended request */
        mdc_open_pack(req, op_data, it->it_create_mode, 0, it->it_flags, lmm,
                      lmmsize);

        /* for remote client, fetch remote perm for current user */
        if (client_is_remote(exp))
                req_capsule_set_size(&req->rq_pill, &RMF_ACL, RCL_SERVER,
                                     sizeof(struct mdt_remote_perm));
        ptlrpc_request_set_replen(req);
        return req;
}

static struct ptlrpc_request *mdc_intent_unlink_pack(struct obd_export *exp,
                                                     struct lookup_intent *it,
                                                     struct md_op_data *op_data)
{
        struct ptlrpc_request *req;
        struct obd_device     *obddev = class_exp2obd(exp);
        struct ldlm_intent    *lit;
        int                    rc;
        ENTRY;

        req = ptlrpc_request_alloc(class_exp2cliimp(exp),
                                   &RQF_LDLM_INTENT_UNLINK);
        if (req == NULL)
                RETURN(ERR_PTR(-ENOMEM));

        mdc_set_capa_size(req, &RMF_CAPA1, op_data->op_capa1);
        req_capsule_set_size(&req->rq_pill, &RMF_NAME, RCL_CLIENT,
                             op_data->op_namelen + 1);

        rc = ptlrpc_request_pack(req, LUSTRE_DLM_VERSION, LDLM_ENQUEUE);
        if (rc) {
                ptlrpc_request_free(req);
                RETURN(ERR_PTR(rc));
        }

        /* pack the intent */
        lit = req_capsule_client_get(&req->rq_pill, &RMF_LDLM_INTENT);
        lit->opc = (__u64)it->it_op;

        /* pack the intended request */
        mdc_unlink_pack(req, op_data);

        req_capsule_set_size(&req->rq_pill, &RMF_MDT_MD, RCL_SERVER,
                             obddev->u.cli.cl_max_mds_easize);
        req_capsule_set_size(&req->rq_pill, &RMF_ACL, RCL_SERVER,
                             obddev->u.cli.cl_max_mds_cookiesize);
        ptlrpc_request_set_replen(req);
        RETURN(req);
}

static struct ptlrpc_request *mdc_intent_getattr_pack(struct obd_export *exp,
                                                      struct lookup_intent *it,
                                                     struct md_op_data *op_data)
{
        struct ptlrpc_request *req;
        struct obd_device     *obddev = class_exp2obd(exp);
        obd_valid              valid = OBD_MD_FLGETATTR | OBD_MD_FLEASIZE |
                                       OBD_MD_FLMODEASIZE | OBD_MD_FLDIREA |
                                       OBD_MD_FLMDSCAPA | OBD_MD_MEA |
                                       (client_is_remote(exp) ?
                                               OBD_MD_FLRMTPERM : OBD_MD_FLACL);
        struct ldlm_intent    *lit;
        int                    rc;
        ENTRY;

        req = ptlrpc_request_alloc(class_exp2cliimp(exp),
                                   &RQF_LDLM_INTENT_GETATTR);
        if (req == NULL)
                RETURN(ERR_PTR(-ENOMEM));

        mdc_set_capa_size(req, &RMF_CAPA1, op_data->op_capa1);
        req_capsule_set_size(&req->rq_pill, &RMF_NAME, RCL_CLIENT,
                             op_data->op_namelen + 1);

        rc = ptlrpc_request_pack(req, LUSTRE_DLM_VERSION, LDLM_ENQUEUE);
        if (rc) {
                ptlrpc_request_free(req);
                RETURN(ERR_PTR(rc));
        }

        /* pack the intent */
        lit = req_capsule_client_get(&req->rq_pill, &RMF_LDLM_INTENT);
        lit->opc = (__u64)it->it_op;

        /* pack the intended request */
        mdc_getattr_pack(req, valid, it->it_flags, op_data);

        req_capsule_set_size(&req->rq_pill, &RMF_MDT_MD, RCL_SERVER,
                             obddev->u.cli.cl_max_mds_easize);
        if (client_is_remote(exp))
                req_capsule_set_size(&req->rq_pill, &RMF_ACL, RCL_SERVER,
                                     sizeof(struct mdt_remote_perm));
        ptlrpc_request_set_replen(req);
        RETURN(req);
}

static struct ptlrpc_request *ldlm_enqueue_pack(struct obd_export *exp)
{
        struct ptlrpc_request *req;
        ENTRY;

        req = ptlrpc_request_alloc_pack(class_exp2cliimp(exp),
                                        &RQF_LDLM_ENQUEUE, LUSTRE_DLM_VERSION,
                                        LDLM_ENQUEUE);
        if (req == NULL)
                RETURN(ERR_PTR(-ENOMEM));

        ptlrpc_request_set_replen(req);
        RETURN(req);
}

/* We always reserve enough space in the reply packet for a stripe MD, because
 * we don't know in advance the file type. */
int mdc_enqueue(struct obd_export *exp, struct ldlm_enqueue_info *einfo,
                struct lookup_intent *it, struct md_op_data *op_data,
                struct lustre_handle *lockh, void *lmm, int lmmsize,
                int extra_lock_flags)
{
        struct obd_device     *obddev = class_exp2obd(exp);
        struct ptlrpc_request *req;
        struct req_capsule    *pill;
        struct ldlm_request   *lockreq;
        struct ldlm_reply     *lockrep;
        int                    flags = extra_lock_flags | LDLM_FL_HAS_INTENT;
        int                    rc;
        struct ldlm_res_id res_id =
                { .name = {fid_seq(&op_data->op_fid1),
                           fid_oid(&op_data->op_fid1),
                           fid_ver(&op_data->op_fid1)} };
        ldlm_policy_data_t policy = { .l_inodebits = { MDS_INODELOCK_LOOKUP } };
        ENTRY;

        LASSERTF(einfo->ei_type == LDLM_IBITS,"lock type %d\n", einfo->ei_type);

        if (it->it_op & (IT_UNLINK | IT_GETATTR | IT_READDIR))
                policy.l_inodebits.bits = MDS_INODELOCK_UPDATE;

        if (it->it_op & IT_OPEN) {
                int joinfile = !!((it->it_flags & O_JOIN_FILE) &&
                                              op_data->op_data);

                req = mdc_intent_open_pack(exp, it, op_data, lmm, lmmsize,
                                           einfo->ei_cbdata);
                if (!joinfile) {
                        policy.l_inodebits.bits = MDS_INODELOCK_UPDATE;
                        einfo->ei_cbdata = NULL;
                        lmm = NULL;
                } else
                        it->it_flags &= ~O_JOIN_FILE;
        } else if (it->it_op & IT_UNLINK)
                req = mdc_intent_unlink_pack(exp, it, op_data);
        else if (it->it_op & (IT_GETATTR | IT_LOOKUP))
                req = mdc_intent_getattr_pack(exp, it, op_data);
        else if (it->it_op == IT_READDIR)
                req = ldlm_enqueue_pack(exp);
        else {
                LBUG();
                RETURN(-EINVAL);
        }

        if (IS_ERR(req))
                RETURN(PTR_ERR(req));
        pill = &req->rq_pill;

        /* It is important to obtain rpc_lock first (if applicable), so that
         * threads that are serialised with rpc_lock are not polluting our
         * rpcs in flight counter */
        mdc_get_rpc_lock(obddev->u.cli.cl_rpc_lock, it);
        mdc_enter_request(&obddev->u.cli);
        rc = ldlm_cli_enqueue(exp, &req, einfo, &res_id, &policy, &flags, NULL,
                              0, NULL, lockh, 0);
        mdc_exit_request(&obddev->u.cli);
        mdc_put_rpc_lock(obddev->u.cli.cl_rpc_lock, it);

        /* Similarly, if we're going to replay this request, we don't want to
         * actually get a lock, just perform the intent. */
        if (req->rq_transno || req->rq_replay) {
                lockreq = req_capsule_client_get(pill, &RMF_DLM_REQ);
                lockreq->lock_flags |= LDLM_FL_INTENT_ONLY;
        }

        if (rc == ELDLM_LOCK_ABORTED) {
                einfo->ei_mode = 0;
                memset(lockh, 0, sizeof(*lockh));
                rc = 0;
        } else if (rc != 0) {
                CERROR("ldlm_cli_enqueue: %d\n", rc);
                LASSERTF(rc < 0, "rc %d\n", rc);
                mdc_clear_replay_flag(req, rc);
                ptlrpc_req_finished(req);
                RETURN(rc);
        } else { /* rc = 0 */
                struct ldlm_lock *lock = ldlm_handle2lock(lockh);
                LASSERT(lock);

                /* If the server gave us back a different lock mode, we should
                 * fix up our variables. */
                if (lock->l_req_mode != einfo->ei_mode) {
                        ldlm_lock_addref(lockh, lock->l_req_mode);
                        ldlm_lock_decref(lockh, einfo->ei_mode);
                        einfo->ei_mode = lock->l_req_mode;
                }
                LDLM_LOCK_PUT(lock);
        }

        lockrep = req_capsule_server_get(pill, &RMF_DLM_REP);
        LASSERT(lockrep != NULL);                 /* checked by ldlm_cli_enqueue() */

        it->d.lustre.it_disposition = (int)lockrep->lock_policy_res1;
        it->d.lustre.it_status = (int)lockrep->lock_policy_res2;
        it->d.lustre.it_lock_mode = einfo->ei_mode;
        it->d.lustre.it_data = req;

        if (it->d.lustre.it_status < 0 && req->rq_replay)
                mdc_clear_replay_flag(req, it->d.lustre.it_status);

        /* If we're doing an IT_OPEN which did not result in an actual
         * successful open, then we need to remove the bit which saves
         * this request for unconditional replay.
         *
         * It's important that we do this first!  Otherwise we might exit the
         * function without doing so, and try to replay a failed create
         * (bug 3440) */
        if (it->it_op & IT_OPEN && req->rq_replay &&
            (!it_disposition(it, DISP_OPEN_OPEN) ||it->d.lustre.it_status != 0))
                mdc_clear_replay_flag(req, it->d.lustre.it_status);

        DEBUG_REQ(D_RPCTRACE, req, "op: %d disposition: %x, status: %d",
                  it->it_op,it->d.lustre.it_disposition,it->d.lustre.it_status);

        /* We know what to expect, so we do any byte flipping required here */
        if (it->it_op & (IT_OPEN | IT_UNLINK | IT_LOOKUP | IT_GETATTR)) {
                struct mdt_body *body;

                body = req_capsule_server_get(pill, &RMF_MDT_BODY);
                if (body == NULL) {
                        CERROR ("Can't swab mdt_body\n");
                        RETURN (-EPROTO);
                }

                if (req->rq_replay && it_disposition(it, DISP_OPEN_OPEN) &&
                    !it_open_error(DISP_OPEN_OPEN, it)) {
                        /*
                         * If this is a successful OPEN request, we need to set
                         * replay handler and data early, so that if replay
                         * happens immediately after swabbing below, new reply
                         * is swabbed by that handler correctly.
                         */
                        mdc_set_open_replay_data(NULL, NULL, req);
                }

                if ((body->valid & (OBD_MD_FLDIREA | OBD_MD_FLEASIZE)) != 0) {
                        void *eadata;

                        /*
                         * The eadata is opaque; just check that it is there.
                         * Eventually, obd_unpackmd() will check the contents.
                         */
                        eadata = req_capsule_server_sized_get(pill, &RMF_MDT_MD,
                                                              body->eadatasize);
                        if (eadata == NULL)
                                RETURN(-EPROTO);

                        if (body->valid & OBD_MD_FLMODEASIZE) {
                                if (obddev->u.cli.cl_max_mds_easize <
                                    body->max_mdsize) {
                                        obddev->u.cli.cl_max_mds_easize =
                                                body->max_mdsize;
                                        CDEBUG(D_INFO, "maxeasize become %d\n",
                                               body->max_mdsize);
                                }
                                if (obddev->u.cli.cl_max_mds_cookiesize <
                                    body->max_cookiesize) {
                                        obddev->u.cli.cl_max_mds_cookiesize =
                                                body->max_cookiesize;
                                        CDEBUG(D_INFO, "cookiesize become %d\n",
                                               body->max_cookiesize);
                                }
                        }

                        /*
                         * We save the reply LOV EA in case we have to replay a
                         * create for recovery.  If we didn't allocate a large
                         * enough request buffer above we need to reallocate it
                         * here to hold the actual LOV EA.
                         *
                         * To not save LOV EA if request is not going to replay
                         * (for example error one).
                         */
                        if ((it->it_op & IT_OPEN) && req->rq_replay) {
                                if (req_capsule_get_size(pill, &RMF_EADATA,
                                                         RCL_CLIENT) <
                                    body->eadatasize) {
                                        mdc_realloc_openmsg(req, body);
                                        req_capsule_set_size(pill, &RMF_EADATA,
                                                             RCL_CLIENT,
                                                             body->eadatasize);
                                }
                                lmm = req_capsule_client_get(pill, &RMF_EADATA);
                                if (lmm)
                                        memcpy(lmm, eadata, body->eadatasize);
                        }
                }

                if (body->valid & OBD_MD_FLRMTPERM) {
                        struct mdt_remote_perm *perm;

                        LASSERT(client_is_remote(exp));
                        perm = req_capsule_server_get(pill, &RMF_ACL);
                        if (perm == NULL)
                                RETURN(-EPROTO);

                        lustre_swab_mdt_remote_perm(perm);
                }
                if (body->valid & OBD_MD_FLMDSCAPA) {
                        struct lustre_capa *capa, *p;

                        capa = req_capsule_server_get(pill, &RMF_CAPA1);
                        if (capa == NULL)
                                RETURN(-EPROTO);

                        if (it->it_op & IT_OPEN) {
                                /* client fid capa will be checked in replay */
                                p = req_capsule_client_get(pill, &RMF_CAPA2);
                                LASSERT(p);
                                *p = *capa;
                        }
                }
                if (body->valid & OBD_MD_FLOSSCAPA) {
                        struct lustre_capa *capa;

                        capa = req_capsule_server_get(pill, &RMF_CAPA2);
                        if (capa == NULL)
                                RETURN(-EPROTO);
                }
        }

        RETURN(rc);
}
/*
 * This long block is all about fixing up the lock and request state
 * so that it is correct as of the moment _before_ the operation was
 * applied; that way, the VFS will think that everything is normal and
 * call Lustre's regular VFS methods.
 *
 * If we're performing a creation, that means that unless the creation
 * failed with EEXIST, we should fake up a negative dentry.
 *
 * For everything else, we want to lookup to succeed.
 *
 * One additional note: if CREATE or OPEN succeeded, we add an extra
 * reference to the request because we need to keep it around until
 * ll_create/ll_open gets called.
 *
 * The server will return to us, in it_disposition, an indication of
 * exactly what d.lustre.it_status refers to.
 *
 * If DISP_OPEN_OPEN is set, then d.lustre.it_status refers to the open() call,
 * otherwise if DISP_OPEN_CREATE is set, then it status is the
 * creation failure mode.  In either case, one of DISP_LOOKUP_NEG or
 * DISP_LOOKUP_POS will be set, indicating whether the child lookup
 * was successful.
 *
 * Else, if DISP_LOOKUP_EXECD then d.lustre.it_status is the rc of the
 * child lookup.
 */
int mdc_intent_lock(struct obd_export *exp, struct md_op_data *op_data,
                    void *lmm, int lmmsize, struct lookup_intent *it,
                    int lookup_flags, struct ptlrpc_request **reqp,
                    ldlm_blocking_callback cb_blocking,
                    int extra_lock_flags)
{
        struct ptlrpc_request *request;
        struct lustre_handle old_lock;
        struct lustre_handle lockh;
        struct mdt_body *mdt_body;
        struct ldlm_lock *lock;
        int rc = 0;
        ENTRY;
        LASSERT(it);

        CDEBUG(D_DLMTRACE, "(name: %.*s,"DFID") in obj "DFID
               ", intent: %s flags %#o\n", op_data->op_namelen,
               op_data->op_name, PFID(&op_data->op_fid2),
               PFID(&op_data->op_fid1), ldlm_it2str(it->it_op),
               it->it_flags);

        if (fid_is_sane(&op_data->op_fid2) &&
            (it->it_op & (IT_LOOKUP | IT_GETATTR))) {
                /* We could just return 1 immediately, but since we should only
                 * be called in revalidate_it if we already have a lock, let's
                 * verify that. */
                ldlm_policy_data_t policy;
                ldlm_mode_t mode;

                /* As not all attributes are kept under update lock, e.g.
                   owner/group/acls are under lookup lock, we need both
                   ibits for GETATTR. */

                /* For CMD, UPDATE lock and LOOKUP lock can not be got
                 * at the same for cross-object, so we can not match
                 * the 2 lock at the same time FIXME: but how to handle
                 * the above situation */
                policy.l_inodebits.bits = (it->it_op == IT_GETATTR) ?
                        MDS_INODELOCK_UPDATE : MDS_INODELOCK_LOOKUP;

                mode = mdc_lock_match(exp, LDLM_FL_BLOCK_GRANTED,
                                      &op_data->op_fid2, LDLM_IBITS, &policy,
                                      LCK_CR|LCK_CW|LCK_PR|LCK_PW, &lockh);
                if (mode) {
                        memcpy(&it->d.lustre.it_lock_handle, &lockh,
                               sizeof(lockh));
                        it->d.lustre.it_lock_mode = mode;
                }

                /* Only return failure if it was not GETATTR by cfid
                   (from inode_revalidate) */
                if (mode || op_data->op_namelen != 0)
                        RETURN(!!mode);
        }

        /* lookup_it may be called only after revalidate_it has run, because
         * revalidate_it cannot return errors, only zero.  Returning zero causes
         * this call to lookup, which *can* return an error.
         *
         * We only want to execute the request associated with the intent one
         * time, however, so don't send the request again.  Instead, skip past
         * this and use the request from revalidate.  In this case, revalidate
         * never dropped its reference, so the refcounts are all OK */
        if (!it_disposition(it, DISP_ENQ_COMPLETE)) {
                struct ldlm_enqueue_info einfo =
                        { LDLM_IBITS, it_to_lock_mode(it), cb_blocking,
                          ldlm_completion_ast, NULL, NULL };

                /* For case if upper layer did not alloc fid, do it now. */
                if (!fid_is_sane(&op_data->op_fid2) && it->it_op & IT_CREAT) {
                        rc = mdc_fid_alloc(exp, &op_data->op_fid2, op_data);
                        if (rc < 0) {
                                CERROR("Can't alloc new fid, rc %d\n", rc);
                                RETURN(rc);
                        }
                }
                rc = mdc_enqueue(exp, &einfo, it, op_data, &lockh,
                                 lmm, lmmsize, extra_lock_flags);
                if (rc < 0)
                        RETURN(rc);
                memcpy(&it->d.lustre.it_lock_handle, &lockh, sizeof(lockh));
        } else if (!fid_is_sane(&op_data->op_fid2) ||
                   !(it->it_flags & O_CHECK_STALE)) {
                /* DISP_ENQ_COMPLETE set means there is extra reference on
                 * request referenced from this intent, saved for subsequent
                 * lookup.  This path is executed when we proceed to this
                 * lookup, so we clear DISP_ENQ_COMPLETE */
                it_clear_disposition(it, DISP_ENQ_COMPLETE);
        }
        request = *reqp = it->d.lustre.it_data;
        LASSERT(request != NULL);
        LASSERT(request != LP_POISON);
        LASSERT(request->rq_repmsg != LP_POISON);

        if (!it_disposition(it, DISP_IT_EXECD)) {
                /* The server failed before it even started executing the
                 * intent, i.e. because it couldn't unpack the request. */
                LASSERT(it->d.lustre.it_status != 0);
                RETURN(it->d.lustre.it_status);
        }
        rc = it_open_error(DISP_IT_EXECD, it);
        if (rc)
                RETURN(rc);

        mdt_body = req_capsule_server_get(&request->rq_pill, &RMF_MDT_BODY);
        LASSERT(mdt_body != NULL);      /* mdc_enqueue checked */

        /* If we were revalidating a fid/name pair, mark the intent in
         * case we fail and get called again from lookup */
        if (fid_is_sane(&op_data->op_fid2) &&
            (it->it_flags & O_CHECK_STALE) &&
            it->it_op != IT_GETATTR) {
                it_set_disposition(it, DISP_ENQ_COMPLETE);

                /* Also: did we find the same inode? */
                if (!lu_fid_eq(&op_data->op_fid2, &mdt_body->fid1))
                        RETURN(-ESTALE);
        }

        rc = it_open_error(DISP_LOOKUP_EXECD, it);
        if (rc)
                RETURN(rc);

        /* keep requests around for the multiple phases of the call
         * this shows the DISP_XX must guarantee we make it into the call
         */
        if (!it_disposition(it, DISP_ENQ_CREATE_REF) &&
            it_disposition(it, DISP_OPEN_CREATE) &&
            !it_open_error(DISP_OPEN_CREATE, it)) {
                it_set_disposition(it, DISP_ENQ_CREATE_REF);
                ptlrpc_request_addref(request); /* balanced in ll_create_node */
        }
        if (!it_disposition(it, DISP_ENQ_OPEN_REF) &&
            it_disposition(it, DISP_OPEN_OPEN) &&
            !it_open_error(DISP_OPEN_OPEN, it)) {
                it_set_disposition(it, DISP_ENQ_OPEN_REF);
                ptlrpc_request_addref(request); /* balanced in ll_file_open */
                /* BUG 11546 - eviction in the middle of open rpc processing */
                OBD_FAIL_TIMEOUT(OBD_FAIL_MDC_ENQUEUE_PAUSE, obd_timeout);
        }

        if (it->it_op & IT_CREAT) {
                /* XXX this belongs in ll_create_it */
        } else if (it->it_op == IT_OPEN) {
                LASSERT(!it_disposition(it, DISP_OPEN_CREATE));
        } else {
                LASSERT(it->it_op & (IT_GETATTR | IT_LOOKUP));
        }

        /* If we already have a matching lock, then cancel the new
         * one.  We have to set the data here instead of in
         * mdc_enqueue, because we need to use the child's inode as
         * the l_ast_data to match, and that's not available until
         * intent_finish has performed the iget().) */
        lock = ldlm_handle2lock(&lockh);
        if (lock) {
                ldlm_policy_data_t policy = lock->l_policy_data;
                LDLM_DEBUG(lock, "matching against this");

                LASSERTF(fid_res_name_eq(&mdt_body->fid1,
                                         &lock->l_resource->lr_name),
                         "Lock res_id: %lu/%lu/%lu, fid: %lu/%lu/%lu.\n",
                         (unsigned long)lock->l_resource->lr_name.name[0],
                         (unsigned long)lock->l_resource->lr_name.name[1],
                         (unsigned long)lock->l_resource->lr_name.name[2],
                         (unsigned long)fid_seq(&mdt_body->fid1),
                         (unsigned long)fid_oid(&mdt_body->fid1),
                         (unsigned long)fid_ver(&mdt_body->fid1));
                LDLM_LOCK_PUT(lock);

                memcpy(&old_lock, &lockh, sizeof(lockh));
                if (ldlm_lock_match(NULL, LDLM_FL_BLOCK_GRANTED, NULL,
                                    LDLM_IBITS, &policy, LCK_NL, &old_lock)) {
                        ldlm_lock_decref_and_cancel(&lockh,
                                                    it->d.lustre.it_lock_mode);
                        memcpy(&lockh, &old_lock, sizeof(old_lock));
                        memcpy(&it->d.lustre.it_lock_handle, &lockh,
                               sizeof(lockh));
                }
        }
        CDEBUG(D_DENTRY,"D_IT dentry %.*s intent: %s status %d disp %x rc %d\n",
               op_data->op_namelen, op_data->op_name, ldlm_it2str(it->it_op),
               it->d.lustre.it_status, it->d.lustre.it_disposition, rc);

        RETURN(rc);
}
