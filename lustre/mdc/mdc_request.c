/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2001-2003 Cluster File Systems, Inc.
 *
 *   This file is part of Lustre, http://www.sf.net/projects/lustre/
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
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

#include <linux/obd_class.h>
#include <linux/lustre_mds.h>
#include <linux/lustre_dlm.h>
#include <linux/lprocfs_status.h>
#include "mdc_internal.h"

#define REQUEST_MINOR 244

static int mdc_cleanup(struct obd_device *obd, int flags);

extern int mds_queue_req(struct ptlrpc_request *);
/* Helper that implements most of mdc_getstatus and signal_completed_replay. */
/* XXX this should become mdc_get_info("key"), sending MDS_GET_INFO RPC */
static int send_getstatus(struct obd_import *imp, struct ll_fid *rootfid,
                          int level, int msg_flags)
{
        struct ptlrpc_request *req;
        struct mds_body *body;
        int rc, size = sizeof(*body);
        ENTRY;

        req = ptlrpc_prep_req(imp, MDS_GETSTATUS, 1, &size, NULL);
        if (!req)
                GOTO(out, rc = -ENOMEM);

        body = lustre_msg_buf(req->rq_reqmsg, 0, sizeof (*body));
        req->rq_send_state = level;
        req->rq_replen = lustre_msg_size(1, &size);

        mdc_pack_req_body(req);
        req->rq_reqmsg->flags |= msg_flags;
        rc = ptlrpc_queue_wait(req);

        if (!rc) {
                body = lustre_swab_repbuf (req, 0, sizeof (*body),
                                           lustre_swab_mds_body);
                if (body == NULL) {
                        CERROR ("Can't extract mds_body\n");
                        GOTO (out, rc = -EPROTO);
                }

                memcpy(rootfid, &body->fid1, sizeof(*rootfid));

                CDEBUG(D_NET, "root ino="LPU64", last_committed="LPU64
                       ", last_xid="LPU64"\n",
                       rootfid->id, req->rq_repmsg->last_committed,
                       req->rq_repmsg->last_xid);
        }

        EXIT;
 out:
        ptlrpc_req_finished(req);
        return rc;
}

/* This should be mdc_get_info("rootfid") */
int mdc_getstatus(struct obd_export *exp, struct ll_fid *rootfid)
{
        return send_getstatus(class_exp2cliimp(exp), rootfid, LUSTRE_IMP_FULL,
                              0);
}

int mdc_getattr_common(struct obd_export *exp, unsigned int ea_size, 
                       struct ptlrpc_request *req)
{
        struct mds_body *body;
        void            *eadata;
        int              rc;
        int              size[2] = {sizeof(*body), 0};
        int              bufcount = 1;
        ENTRY;

        /* request message already built */

        if (ea_size != 0) {
                size[bufcount++] = ea_size;
                CDEBUG(D_INODE, "reserved %u bytes for MD/symlink in packet\n",
                       ea_size);
        }
        req->rq_replen = lustre_msg_size(bufcount, size);

        mdc_get_rpc_lock(exp->exp_obd->u.cli.cl_rpc_lock, NULL);
        rc = ptlrpc_queue_wait(req);
        mdc_put_rpc_lock(exp->exp_obd->u.cli.cl_rpc_lock, NULL);
        if (rc != 0)
                RETURN (rc);

        body = lustre_swab_repbuf (req, 0, sizeof (*body),
                                   lustre_swab_mds_body);
        if (body == NULL) {
                CERROR ("Can't unpack mds_body\n");
                RETURN (-EPROTO);
        }

        CDEBUG(D_NET, "mode: %o\n", body->mode);

        LASSERT_REPSWAB (req, 1);
        if (body->eadatasize != 0) {
                /* reply indicates presence of eadata; check it's there... */
                eadata = lustre_msg_buf (req->rq_repmsg, 1, body->eadatasize);
                if (eadata == NULL) {
                        CERROR ("Missing/short eadata\n");
                        RETURN (-EPROTO);
                }
        }

        RETURN (0);
}

int mdc_getattr(struct obd_export *exp, struct ll_fid *fid,
                unsigned long valid, unsigned int ea_size,
                struct ptlrpc_request **request)
{
        struct ptlrpc_request *req;
        struct mds_body *body;
        int size = sizeof(*body);
        int rc;
        ENTRY;

        /* XXX do we need to make another request here?  We just did a getattr
         *     to do the lookup in the first place.
         */
        req = ptlrpc_prep_req(class_exp2cliimp(exp), MDS_GETATTR, 1, &size,
                              NULL);
        if (!req)
                GOTO(out, rc = -ENOMEM);

        body = lustre_msg_buf(req->rq_reqmsg, 0, sizeof (*body));
        memcpy(&body->fid1, fid, sizeof(*fid));
        body->valid = valid;
        body->eadatasize = ea_size;
        mdc_pack_req_body(req);

        rc = mdc_getattr_common(exp, ea_size, req);
        if (rc != 0) {
                ptlrpc_req_finished (req);
                req = NULL;
        }
 out:
        *request = req;
        RETURN (rc);
}

int mdc_getattr_name(struct obd_export *exp, struct ll_fid *fid,
                     char *filename, int namelen, unsigned long valid,
                     unsigned int ea_size, struct ptlrpc_request **request)
{
        struct ptlrpc_request *req;
        struct mds_body *body;
        int rc, size[2] = {sizeof(*body), namelen};
        ENTRY;

        req = ptlrpc_prep_req(class_exp2cliimp(exp), MDS_GETATTR_NAME, 2,
                              size, NULL);
        if (!req)
                GOTO(out, rc = -ENOMEM);

        body = lustre_msg_buf(req->rq_reqmsg, 0, sizeof (*body));
        memcpy(&body->fid1, fid, sizeof(*fid));
        body->valid = valid;
        body->eadatasize = ea_size;
        mdc_pack_req_body(req);

        LASSERT (strnlen (filename, namelen) == namelen - 1);
        memcpy(lustre_msg_buf(req->rq_reqmsg, 1, namelen), filename, namelen);

        rc = mdc_getattr_common(exp, ea_size, req);
        if (rc != 0) {
                ptlrpc_req_finished (req);
                req = NULL;
        }
 out:
        *request = req;
        RETURN(rc);
}

/* This should be called with both the request and the reply still packed. */
void mdc_store_inode_generation(struct ptlrpc_request *req, int reqoff,
                                int repoff)
{
        struct mds_rec_create *rec =
                lustre_msg_buf(req->rq_reqmsg, reqoff, sizeof(*rec));
        struct mds_body *body =
                lustre_msg_buf(req->rq_repmsg, repoff, sizeof(*body));

        LASSERT (rec != NULL);
        LASSERT (body != NULL);

        memcpy(&rec->cr_replayfid, &body->fid1, sizeof rec->cr_replayfid);
        DEBUG_REQ(D_HA, req, "storing generation %u for ino "LPU64,
                  rec->cr_replayfid.generation, rec->cr_replayfid.id);
}

int mdc_req2lustre_md(struct ptlrpc_request *req, int offset,
                      struct obd_export *exp_osc,
                      struct obd_export *exp_mdc,
                      struct lustre_md *md)
{
        int rc = 0;
        ENTRY;

        LASSERT(md);
        memset(md, 0, sizeof(*md));

        md->body = lustre_msg_buf(req->rq_repmsg, offset, sizeof (*md->body));
        LASSERT (md->body != NULL);
        LASSERT_REPSWABBED (req, offset);

        if (!(md->body->valid & OBD_MD_FLEASIZE))
                RETURN(0);

        /* ea is presented in reply, parse it */
        if (S_ISREG(md->body->mode)) {
                int lmmsize;
                struct lov_mds_md *lmm;

                if (md->body->eadatasize == 0) {
                        CERROR ("OBD_MD_FLEASIZE set, but eadatasize 0\n");
                        RETURN(-EPROTO);
                }
                lmmsize = md->body->eadatasize;
                lmm = lustre_msg_buf(req->rq_repmsg, offset + 1, lmmsize);
                LASSERT (lmm != NULL);
                LASSERT_REPSWABBED (req, offset + 1);

                rc = obd_unpackmd(exp_osc, &md->lsm, lmm, lmmsize);
                if (rc >= 0) {
                        LASSERT (rc >= sizeof (*md->lsm));
                        rc = 0;
                }
        } else if (S_ISDIR(md->body->mode)) {
                struct mea *mea;
                int mdsize;
                LASSERT(exp_mdc != NULL);
                /* dir can be non-splitted */
                if (md->body->eadatasize == 0)
                        RETURN(0);

                mdsize = md->body->eadatasize;
                mea = lustre_msg_buf(req->rq_repmsg, offset + 1, mdsize);
                LASSERT(mea != NULL);

                rc = obd_unpackmd(exp_mdc, (void *) &md->mea,
                                  (void *) mea, mdsize);
                if (rc >= 0) {
                        LASSERT (rc >= sizeof (*md->mea));
                        rc = 0;
                }
        } else {
                LASSERT(0);
        }
        RETURN(rc);
}

static void mdc_commit_open(struct ptlrpc_request *req)
{
        struct mdc_open_data *mod = req->rq_cb_data;
        if (mod == NULL)
                return;

        if (mod->mod_close_req != NULL)
                mod->mod_close_req->rq_cb_data = NULL;

        if (mod->mod_och != NULL)
                mod->mod_och->och_mod = NULL;

        OBD_FREE(mod, sizeof(*mod));
        req->rq_cb_data = NULL;
}

static void mdc_replay_open(struct ptlrpc_request *req)
{
        struct mdc_open_data *mod = req->rq_cb_data;
        struct obd_client_handle *och;
        struct ptlrpc_request *close_req;
        struct lustre_handle old; 
        struct mds_body *body;
        ENTRY;

        body = lustre_swab_repbuf(req, 1, sizeof(*body), lustre_swab_mds_body);
        LASSERT (body != NULL);

        if (mod == NULL) {
                DEBUG_REQ(D_ERROR, req,
                          "can't properly replay without open data");
                EXIT;
                return;
        }

        och = mod->mod_och;
        if (och != NULL) {
                struct lustre_handle *file_fh; 
                LASSERT(och->och_magic == OBD_CLIENT_HANDLE_MAGIC);
                file_fh = &och->och_fh;
                CDEBUG(D_HA, "updating handle from "LPX64" to "LPX64"\n",
                       file_fh->cookie, body->handle.cookie);
                memcpy(&old, file_fh, sizeof(old));
                memcpy(file_fh, &body->handle, sizeof(*file_fh));
        } 

        close_req = mod->mod_close_req;
        if (close_req != NULL) {
                struct mds_body *close_body;
                LASSERT(close_req->rq_reqmsg->opc == MDS_CLOSE);
                close_body = lustre_msg_buf(close_req->rq_reqmsg, 0,
                                            sizeof(*close_body));
                if (och != NULL)
                        LASSERT(!memcmp(&old, &close_body->handle, sizeof old));
                DEBUG_REQ(D_HA, close_req, "updating close body with new fh");
                memcpy(&close_body->handle, &body->handle,
                       sizeof(close_body->handle));
        }

        EXIT;
}

void mdc_set_open_replay_data(struct obd_client_handle *och,
                              struct ptlrpc_request *open_req)
{
        struct mdc_open_data *mod;
        struct mds_rec_create *rec =
                lustre_msg_buf(open_req->rq_reqmsg, 2, sizeof(*rec));
        struct mds_body *body =
                lustre_msg_buf(open_req->rq_repmsg, 1, sizeof(*body));

        LASSERT(rec != NULL);
        /* outgoing messages always in my byte order */
        LASSERT(body != NULL);
        /* incoming message in my byte order (it's been swabbed) */
        LASSERT_REPSWABBED(open_req, 1);

        OBD_ALLOC(mod, sizeof(*mod));
        if (mod == NULL) {
                DEBUG_REQ(D_ERROR, open_req, "can't allocate mdc_open_data");
                return;
        }

        och->och_mod = mod;
        mod->mod_och = och;
        mod->mod_open_req = open_req;

        memcpy(&rec->cr_replayfid, &body->fid1, sizeof rec->cr_replayfid);
        open_req->rq_replay_cb = mdc_replay_open;
        open_req->rq_commit_cb = mdc_commit_open;
        open_req->rq_cb_data = mod;
        DEBUG_REQ(D_HA, open_req, "set up replay data");
}

void mdc_clear_open_replay_data(struct obd_client_handle *och)
{
        struct mdc_open_data *mod = och->och_mod;

        /* Don't free the structure now (it happens in mdc_commit_open, after
         * we're sure we won't need to fix up the close request in the future),
         * but make sure that replay doesn't poke at the och, which is about to
         * be freed. */
        LASSERT(mod != (void *)0x5a5a5a5a);
        if (mod != NULL)
                mod->mod_och = NULL;
        och->och_mod = NULL;
}

static void mdc_commit_close(struct ptlrpc_request *req)
{
        struct mdc_open_data *mod = req->rq_cb_data;
        struct ptlrpc_request *open_req;
        struct obd_import *imp = req->rq_import;

        DEBUG_REQ(D_HA, req, "close req committed");
        if (mod == NULL)
                return;

        mod->mod_close_req = NULL;
        req->rq_cb_data = NULL;
        req->rq_commit_cb = NULL;

        open_req = mod->mod_open_req;
        LASSERT(open_req != NULL);
        LASSERT(open_req != (void *)0x5a5a5a5a);

        DEBUG_REQ(D_HA, open_req, "open req balanced");
        LASSERT(open_req->rq_transno != 0);
        LASSERT(open_req->rq_import == imp);

        /* We no longer want to preserve this for transno-unconditional
         * replay. */
        spin_lock(&open_req->rq_lock);
        open_req->rq_replay = 0;
        spin_unlock(&open_req->rq_lock);
}

static int mdc_close_interpret(struct ptlrpc_request *req, void *data, int rc)
{
        union ptlrpc_async_args *aa = data;
        struct mdc_rpc_lock *rpc_lock = aa->pointer_arg[0];
        struct obd_device *obd = aa->pointer_arg[1];

        if (rpc_lock == NULL) {
                CERROR("called with NULL rpc_lock\n");
        } else {
                mdc_put_rpc_lock(rpc_lock, NULL);
                LASSERTF(req->rq_async_args.pointer_arg[0] ==
                         obd->u.cli.cl_rpc_lock, "%p != %p\n",
                         req->rq_async_args.pointer_arg[0],
                         obd->u.cli.cl_rpc_lock);
                aa->pointer_arg[0] = NULL;
        }
        wake_up(&req->rq_reply_waitq);
        RETURN(rc);
}

/* We can't use ptlrpc_check_reply, because we don't want to wake up for
 * anything but a reply or an error. */
static int mdc_close_check_reply(struct ptlrpc_request *req)
{
        int rc = 0;
        unsigned long flags;

        spin_lock_irqsave(&req->rq_lock, flags);
        if (PTLRPC_REQUEST_COMPLETE(req)) {
                rc = 1;
        }
        spin_unlock_irqrestore (&req->rq_lock, flags);
        return rc;
}

static int go_back_to_sleep(void *unused)
{
        return 0;
}

int mdc_close(struct obd_export *exp, struct obdo *obdo,
              struct obd_client_handle *och, struct ptlrpc_request **request)
{
        struct mds_body *body;
        struct obd_device *obd = class_exp2obd(exp);
        int reqsize = sizeof(*body);
        int rc, repsize[3] = {sizeof(*body),
                              obd->u.cli.cl_max_mds_easize,
                              obd->u.cli.cl_max_mds_cookiesize};
        struct ptlrpc_request *req;
        struct mdc_open_data *mod;
        struct l_wait_info lwi;
        ENTRY;

        req = ptlrpc_prep_req(class_exp2cliimp(exp), MDS_CLOSE, 1, &reqsize,
                              NULL);
        if (req == NULL)
                GOTO(out, rc = -ENOMEM);

        /* Ensure that this close's handle is fixed up during replay. */
        LASSERT(och != NULL);
        mod = och->och_mod;
        if (likely(mod != NULL)) {
                mod->mod_close_req = req;
                DEBUG_REQ(D_HA, mod->mod_open_req, "matched open req %p",
                          mod->mod_open_req);
        } else {
                CDEBUG(D_HA, "couldn't find open req; expecting close error\n");
        }

        body = lustre_msg_buf(req->rq_reqmsg, 0, sizeof(*body));
        mdc_pack_fid(&body->fid1, obdo->o_id, 0, obdo->o_mode);
        memcpy(&body->handle, &och->och_fh, sizeof(body->handle));
        body->size = obdo->o_size;
        body->blocks = obdo->o_blocks;
        body->flags = obdo->o_flags;
        body->valid = obdo->o_valid;

        req->rq_replen = lustre_msg_size(3, repsize);
        req->rq_commit_cb = mdc_commit_close;
        LASSERT(req->rq_cb_data == NULL);
        req->rq_cb_data = mod;

        /* We hand a ref to the rpcd here, so we need another one of our own. */
        ptlrpc_request_addref(req);

        mdc_get_rpc_lock(obd->u.cli.cl_rpc_lock, NULL);
        req->rq_interpret_reply = mdc_close_interpret;
        req->rq_async_args.pointer_arg[0] = obd->u.cli.cl_rpc_lock;
        req->rq_async_args.pointer_arg[1] = obd;
        ptlrpcd_add_req(req);
        lwi = LWI_TIMEOUT_INTR(MAX(req->rq_timeout * HZ, 1), go_back_to_sleep,
                               NULL, NULL);
        rc = l_wait_event(req->rq_reply_waitq, mdc_close_check_reply(req),
                          &lwi);
        if (rc == 0) {
                rc = req->rq_repmsg->status;
                if (req->rq_repmsg->type == PTL_RPC_MSG_ERR) {
                        DEBUG_REQ(D_ERROR, req, "type == PTL_RPC_MSG_ERR, err "
                                  "= %d", rc);
                        if (rc > 0)
                                rc = -rc;
                } else if (mod == NULL) {
                        CERROR("Unexpected: can't find mdc_open_data, but the "
                               "close succeeded.  Please tell CFS.\n");
                }
                if (!lustre_swab_repbuf(req, 0, sizeof(struct mds_body),
                                        lustre_swab_mds_body)) {
                        CERROR("Error unpacking mds_body\n");
                        rc = -EPROTO;
                }
        }
        if (req->rq_async_args.pointer_arg[0] != NULL) {
                CERROR("returned without dropping rpc_lock: rc %d\n", rc);
                mdc_close_interpret(req, &req->rq_async_args, rc);
                portals_debug_dumplog();
        }

        EXIT;
 out:
        *request = req;
        return rc;
}

int mdc_done_writing(struct obd_export *exp, struct obdo *obdo)
{
        struct ptlrpc_request *req;
        struct mds_body *body;
        int rc, size = sizeof(*body);
        ENTRY;

        req = ptlrpc_prep_req(class_exp2cliimp(exp), MDS_DONE_WRITING, 1,
                              &size, NULL);
        if (req == NULL)
                RETURN(-ENOMEM);

        body = lustre_msg_buf(req->rq_reqmsg, 0, sizeof(*body));
        mdc_pack_fid(&body->fid1, obdo->o_id, 0, obdo->o_mode);
        body->size = obdo->o_size;
        body->blocks = obdo->o_blocks;
        body->flags = obdo->o_flags;
        body->valid = obdo->o_valid;
//        memcpy(&body->handle, &och->och_fh, sizeof(body->handle));

        req->rq_replen = lustre_msg_size(1, &size);

        rc = ptlrpc_queue_wait(req);
        ptlrpc_req_finished(req);
        RETURN(rc);
}

int mdc_readpage(struct obd_export *exp, struct ll_fid *mdc_fid,
                 __u64 offset, struct page *page,
                 struct ptlrpc_request **request)
{
        struct obd_import *imp = class_exp2cliimp(exp);
        struct ptlrpc_request *req = NULL;
        struct ptlrpc_bulk_desc *desc = NULL;
        struct mds_body *body;
        int rc, size = sizeof(*body);
        ENTRY;

        CDEBUG(D_INODE, "inode: %ld\n", (long)mdc_fid->id);

        req = ptlrpc_prep_req(imp, MDS_READPAGE, 1, &size, NULL);
        if (req == NULL)
                GOTO(out, rc = -ENOMEM);
        /* XXX FIXME bug 249 */
        req->rq_request_portal = MDS_READPAGE_PORTAL;

        desc = ptlrpc_prep_bulk_imp(req, 1, BULK_PUT_SINK, MDS_BULK_PORTAL);
        if (desc == NULL)
                GOTO(out, rc = -ENOMEM);
        /* NB req now owns desc and will free it when it gets freed */

        ptlrpc_prep_bulk_page(desc, page, 0, PAGE_CACHE_SIZE);

        mdc_readdir_pack(req, offset, PAGE_CACHE_SIZE, mdc_fid);

        req->rq_replen = lustre_msg_size(1, &size);
        rc = ptlrpc_queue_wait(req);

        if (rc == 0) {
                body = lustre_swab_repbuf(req, 0, sizeof (*body),
                                          lustre_swab_mds_body);
                if (body == NULL) {
                        CERROR("Can't unpack mds_body\n");
                        GOTO(out, rc = -EPROTO);
                }

                if (req->rq_bulk->bd_nob_transferred != PAGE_CACHE_SIZE) {
                        CERROR ("Unexpected # bytes transferred: %d"
                                " (%ld expected)\n",
                                req->rq_bulk->bd_nob_transferred,
                                PAGE_CACHE_SIZE);
                        GOTO (out, rc = -EPROTO);
                }
        }

        EXIT;
 out:
        *request = req;
        return rc;
}

static int mdc_iocontrol(unsigned int cmd, struct obd_export *exp, int len,
                         void *karg, void *uarg)
{
        struct obd_device *obd = exp->exp_obd;
        struct obd_ioctl_data *data = karg;
        struct obd_import *imp = obd->u.cli.cl_import;
        struct llog_ctxt *ctxt;
        int rc;
        ENTRY;
        
        MOD_INC_USE_COUNT;

        switch (cmd) {
        case OBD_IOC_CLIENT_RECOVER:
                rc = ptlrpc_recover_import(imp, data->ioc_inlbuf1);
                if (rc < 0)
                        GOTO(out, rc);
                GOTO(out, rc = 0);
        case IOC_OSC_SET_ACTIVE:
                rc = ptlrpc_set_import_active(imp, data->ioc_offset);
                GOTO(out, rc);
        case OBD_IOC_PARSE: {
                ctxt = llog_get_context(&exp->exp_obd->obd_llogs,
                                        LLOG_CONFIG_REPL_CTXT);
                rc = class_config_parse_llog(ctxt, data->ioc_inlbuf1, NULL);
                GOTO(out, rc);
        }
#ifdef __KERNEL__
        case OBD_IOC_LLOG_INFO:
        case OBD_IOC_LLOG_PRINT: {
                ctxt = llog_get_context(&obd->obd_llogs, LLOG_CONFIG_REPL_CTXT);
                rc = llog_ioctl(ctxt, cmd, data);
                
                GOTO(out, rc);
        }
#endif
        default:
                CERROR("osc_ioctl(): unrecognised ioctl %#x\n", cmd);
                GOTO(out, rc = -ENOTTY);
        }
out:
        MOD_DEC_USE_COUNT;
        return rc;
}

int mdc_set_info(struct obd_export *exp, obd_count keylen,
                 void *key, obd_count vallen, void *val)
{
        int rc = -EINVAL;

        if (keylen == strlen("initial_recov") &&
            memcmp(key, "initial_recov", strlen("initial_recov")) == 0) {
                struct obd_import *imp = exp->exp_obd->u.cli.cl_import;
                if (vallen != sizeof(int))
                        RETURN(-EINVAL);
                imp->imp_initial_recov = *(int *)val;
                CDEBUG(D_HA, "%s: set imp_no_init_recov = %d\n",
                       exp->exp_obd->obd_name,
                       imp->imp_initial_recov);
                RETURN(0);
        } else if (keylen >= strlen("client") && strcmp(key, "client") == 0) {
                struct ptlrpc_request *req;
                char *bufs[1] = {key};
                int rc;
                req = ptlrpc_prep_req(class_exp2cliimp(exp), OST_SET_INFO, 1,
                                      &keylen, bufs);
                if (req == NULL)
                        RETURN(-ENOMEM);

                req->rq_replen = lustre_msg_size(0, NULL);
                rc = ptlrpc_queue_wait(req);
                ptlrpc_req_finished(req);
                RETURN(rc);
        }
        
        RETURN(rc);
}

static int mdc_statfs(struct obd_device *obd, struct obd_statfs *osfs,
                      unsigned long max_age)
{
        struct ptlrpc_request *req;
        struct obd_statfs *msfs;
        int rc, size = sizeof(*msfs);
        ENTRY;

        /* We could possibly pass max_age in the request (as an absolute
         * timestamp or a "seconds.usec ago") so the target can avoid doing
         * extra calls into the filesystem if that isn't necessary (e.g.
         * during mount that would help a bit).  Having relative timestamps
         * is not so great if request processing is slow, while absolute
         * timestamps are not ideal because they need time synchronization. */
        req = ptlrpc_prep_req(obd->u.cli.cl_import, MDS_STATFS, 0, NULL, NULL);
        if (!req)
                RETURN(-ENOMEM);

        req->rq_replen = lustre_msg_size(1, &size);

        mdc_get_rpc_lock(obd->u.cli.cl_rpc_lock, NULL);
        rc = ptlrpc_queue_wait(req);
        mdc_put_rpc_lock(obd->u.cli.cl_rpc_lock, NULL);

        if (rc)
                GOTO(out, rc);

        msfs = lustre_swab_repbuf(req, 0, sizeof(*msfs),lustre_swab_obd_statfs);
        if (msfs == NULL) {
                CERROR("Can't unpack obd_statfs\n");
                GOTO(out, rc = -EPROTO);
        }

        memcpy(osfs, msfs, sizeof (*msfs));
        EXIT;
out:
        ptlrpc_req_finished(req);

        return rc;
}

static int mdc_pin(struct obd_export *exp, obd_id ino, __u32 gen, int type,
                   struct obd_client_handle *handle, int flag)
{
        struct ptlrpc_request *req;
        struct mds_body *body;
        int rc, size = sizeof(*body);
        ENTRY;

        req = ptlrpc_prep_req(class_exp2cliimp(exp), MDS_PIN, 1, &size, NULL);
        if (req == NULL)
                RETURN(-ENOMEM);

        body = lustre_msg_buf(req->rq_reqmsg, 0, sizeof (*body));
        mdc_pack_fid(&body->fid1, ino, gen, type);
        body->flags = flag;

        req->rq_replen = lustre_msg_size(1, &size);

        mdc_get_rpc_lock(exp->exp_obd->u.cli.cl_rpc_lock, NULL);
        rc = ptlrpc_queue_wait(req);
        mdc_put_rpc_lock(exp->exp_obd->u.cli.cl_rpc_lock, NULL);
        if (rc) {
                CERROR("pin failed: %d\n", rc);
                ptlrpc_req_finished(req);
                RETURN(rc);
        }

        body = lustre_swab_repbuf(req, 0, sizeof(*body), lustre_swab_mds_body);
        if (body == NULL) {
                ptlrpc_req_finished(req);
                RETURN(rc);
        }

        memcpy(&handle->och_fh, &body->handle, sizeof(body->handle));
        handle->och_magic = OBD_CLIENT_HANDLE_MAGIC;

        OBD_ALLOC(handle->och_mod, sizeof(*handle->och_mod));
        if (handle->och_mod == NULL) {
                DEBUG_REQ(D_ERROR, req, "can't allocate mdc_open_data");
                RETURN(-ENOMEM);
        }
        handle->och_mod->mod_open_req = req; /* will be dropped by unpin */

        RETURN(rc);
}

static int mdc_unpin(struct obd_export *exp,
                     struct obd_client_handle *handle, int flag)
{
        struct ptlrpc_request *req;
        struct mds_body *body;
        int rc, size = sizeof(*body);
        ENTRY;

        if (handle->och_magic != OBD_CLIENT_HANDLE_MAGIC)
                RETURN(0);

        req = ptlrpc_prep_req(class_exp2cliimp(exp), MDS_CLOSE, 1, &size, NULL);
        if (req == NULL)
                RETURN(-ENOMEM);

        body = lustre_msg_buf(req->rq_reqmsg, 0, sizeof(*body));
        memcpy(&body->handle, &handle->och_fh, sizeof(body->handle));
        body->flags = flag;

        req->rq_replen = lustre_msg_size(0, NULL);
        mdc_get_rpc_lock(exp->exp_obd->u.cli.cl_rpc_lock, NULL);
        rc = ptlrpc_queue_wait(req);
        mdc_put_rpc_lock(exp->exp_obd->u.cli.cl_rpc_lock, NULL);

        if (rc != 0)
                CERROR("unpin failed: %d\n", rc);

        ptlrpc_req_finished(req);
        ptlrpc_req_finished(handle->och_mod->mod_open_req);
        OBD_FREE(handle->och_mod, sizeof(*handle->och_mod));
        RETURN(rc);
}

int mdc_sync(struct obd_export *exp, struct ll_fid *fid,
             struct ptlrpc_request **request)
{
        struct ptlrpc_request *req;
        struct mds_body *body;
        int size = sizeof(*body);
        int rc;
        ENTRY;

        req = ptlrpc_prep_req(class_exp2cliimp(exp), MDS_SYNC, 1,&size,NULL);
        if (!req)
                RETURN(rc = -ENOMEM);

        if (fid) {
                body = lustre_msg_buf(req->rq_reqmsg, 0, sizeof (*body));
                memcpy(&body->fid1, fid, sizeof(*fid));
                mdc_pack_req_body(req);
        }

        req->rq_replen = lustre_msg_size(1, &size);

        rc = ptlrpc_queue_wait(req);
        if (rc || request == NULL)
                ptlrpc_req_finished(req);
        else
                *request = req;

        RETURN(rc);
}

static int mdc_attach(struct obd_device *dev, obd_count len, void *data)
{
        struct lprocfs_static_vars lvars;

        lprocfs_init_vars(mdc, &lvars);
        return lprocfs_obd_attach(dev, lvars.obd_vars);
}

static int mdc_detach(struct obd_device *dev)
{
        return lprocfs_obd_detach(dev);
}

static int mdc_import_event(struct obd_device *obd,
                            struct obd_import *imp, 
                            enum obd_import_event event)
{
        int rc = 0;

        LASSERT(imp->imp_obd == obd);

        switch (event) {
        case IMP_EVENT_DISCON: {
                break;
        }
        case IMP_EVENT_INACTIVE: {
                if (obd->obd_observer)
                        rc = obd_notify(obd->obd_observer, obd, 0);
                break;
        }
        case IMP_EVENT_INVALIDATE: {
                struct ldlm_namespace *ns = obd->obd_namespace;
                
                ldlm_namespace_cleanup(ns, LDLM_FL_LOCAL_ONLY);

                break;
        }
        case IMP_EVENT_ACTIVE: {
                if (obd->obd_observer)
                        rc = obd_notify(obd->obd_observer, obd, 1);
                break;
        }
        default:
                CERROR("Unknown import event %d\n", event);
                LBUG();
        }
        RETURN(rc);
}

static int mdc_setup(struct obd_device *obd, obd_count len, void *buf)
{
        struct client_obd *cli = &obd->u.cli;
        int rc;
        ENTRY;

        OBD_ALLOC(cli->cl_rpc_lock, sizeof (*cli->cl_rpc_lock));
        if (!cli->cl_rpc_lock)
                RETURN(-ENOMEM);
        mdc_init_rpc_lock(cli->cl_rpc_lock);

        ptlrpcd_addref();

        OBD_ALLOC(cli->cl_setattr_lock, sizeof (*cli->cl_setattr_lock));
        if (!cli->cl_setattr_lock)
                GOTO(out_free_rpc, rc = -ENOMEM);
        mdc_init_rpc_lock(cli->cl_setattr_lock);

        rc = client_obd_setup(obd, len, buf);
        if (rc) {
                OBD_FREE(cli->cl_setattr_lock, sizeof (*cli->cl_setattr_lock));
 out_free_rpc:
                OBD_FREE(cli->cl_rpc_lock, sizeof (*cli->cl_rpc_lock));
        }

        rc = obd_llog_init(obd, &obd->obd_llogs, obd, 0, NULL);
        if (rc) {
                mdc_cleanup(obd, 0);
                CERROR("failed to setup llogging subsystems\n");
        }

        RETURN(rc);
}

int mdc_init_ea_size(struct obd_export *exp, int easize, int cookiesize)
{
        struct obd_device *obd = exp->exp_obd;
        struct client_obd *cli = &obd->u.cli;
        ENTRY;

        if (cli->cl_max_mds_easize < easize)
                cli->cl_max_mds_easize = easize;
        if (cli->cl_max_mds_cookiesize < cookiesize)
                cli->cl_max_mds_cookiesize = cookiesize;
        RETURN(0);
}

static int mdc_precleanup(struct obd_device *obd, int flags)
{
        int rc = 0;
        
        rc = obd_llog_finish(obd, &obd->obd_llogs, 0);
        if (rc != 0)
                CERROR("failed to cleanup llogging subsystems\n");

        RETURN(rc);
}

static int mdc_cleanup(struct obd_device *obd, int flags)
{
        struct client_obd *cli = &obd->u.cli;
 
        OBD_FREE(cli->cl_rpc_lock, sizeof (*cli->cl_rpc_lock));
        OBD_FREE(cli->cl_setattr_lock, sizeof (*cli->cl_setattr_lock));

        ptlrpcd_decref();

        return client_obd_cleanup(obd, flags);
}


static int mdc_llog_init(struct obd_device *obd, struct obd_llogs *llogs, 
                         struct obd_device *tgt, int count,
                         struct llog_catid *logid)
{
        struct llog_ctxt *ctxt;
        int rc;
        ENTRY;

        rc = llog_setup(obd, llogs, LLOG_CONFIG_REPL_CTXT, tgt, 0, NULL,
                        &llog_client_ops);
        if (rc == 0) {
                ctxt = llog_get_context(llogs, LLOG_CONFIG_REPL_CTXT);
                ctxt->loc_imp = obd->u.cli.cl_import;
        }

        RETURN(rc);
}

static int mdc_llog_finish(struct obd_device *obd,
                           struct obd_llogs *llogs, int count)
{
        int rc;
        ENTRY;

        rc = llog_cleanup(llog_get_context(llogs, LLOG_CONFIG_REPL_CTXT));
        RETURN(rc);
}

int mdc_obj_create(struct obd_export *exp, struct obdo *oa,
                    struct lov_stripe_md **ea, struct obd_trans_info *oti)
{
        struct ptlrpc_request *request;
        struct ost_body *body;
        int rc, size = sizeof(*body);
        ENTRY;

        LASSERT(oa);

        request = ptlrpc_prep_req(class_exp2cliimp(exp), OST_CREATE,
                                  1, &size, NULL);
        if (!request)
                GOTO(out_req, rc = -ENOMEM);

        body = lustre_msg_buf(request->rq_reqmsg, 0, sizeof (*body));
        memcpy(&body->oa, oa, sizeof(body->oa));

        request->rq_replen = lustre_msg_size(1, &size);
        rc = ptlrpc_queue_wait(request);
        if (rc)
                GOTO(out_req, rc);

        body = lustre_swab_repbuf(request, 0, sizeof(*body),
                                  lustre_swab_ost_body);
        if (body == NULL) {
                CERROR ("can't unpack ost_body\n");
                GOTO (out_req, rc = -EPROTO);
        }

        memcpy(oa, &body->oa, sizeof(*oa));

        /* store ino/generation for recovery */
        body = lustre_msg_buf(request->rq_reqmsg, 0, sizeof (*body));
        body->oa.o_id = oa->o_id;
        body->oa.o_generation = oa->o_generation;

        CDEBUG(D_HA, "transno: "LPD64"\n", request->rq_repmsg->transno);
        EXIT;
out_req:
        ptlrpc_req_finished(request);
        return rc;
}

static int mdc_get_info(struct obd_export *exp, obd_count keylen,
                        void *key, __u32 *vallen, void *val)
{
        ENTRY;
        if (!vallen || !val)
                RETURN(-EFAULT);

        if (keylen >= strlen("mdsize") && strcmp(key, "mdsize") == 0) {
                struct ptlrpc_request *req;
                __u32 *reply;
                char *bufs[1] = {key};
                int rc;
                req = ptlrpc_prep_req(class_exp2cliimp(exp), OST_GET_INFO, 1,
                                      &keylen, bufs);
                if (req == NULL)
                        RETURN(-ENOMEM);

                req->rq_replen = lustre_msg_size(1, vallen);
                rc = ptlrpc_queue_wait(req);
                if (rc)
                        GOTO(out, rc);

                /* FIXME: right swap routine here! */
                reply = lustre_swab_repbuf(req, 0, sizeof(*reply), NULL);
                if (reply == NULL) {
                        CERROR("Can't unpack mdsize\n");
                        GOTO(out, rc = -EPROTO);
                }
                *((__u32 *)val) = *reply;
        out:
                ptlrpc_req_finished(req);
                RETURN(rc);
        }
        RETURN(-EINVAL);
}

int mdc_brw(int rw, struct obd_export *exp, struct obdo *oa,
                struct lov_stripe_md *ea, obd_count oa_bufs,
                struct brw_page *pgarr, struct obd_trans_info *oti)
{
        struct ptlrpc_bulk_desc *desc;
        struct niobuf_remote *niobuf;
        struct ptlrpc_request *req;
        struct obd_ioobj *ioobj;
        struct ost_body *body;
        int err, opc, i;
        int size[3];

        opc = ((rw & OBD_BRW_WRITE) != 0) ? OST_WRITE : OST_READ;
        
        size[0] = sizeof(*body);
        size[1] = sizeof(*ioobj);
        size[2] = oa_bufs * sizeof(*niobuf);

        req = ptlrpc_prep_req(class_exp2cliimp(exp), opc, 3, size, NULL);
        LASSERT(req != NULL);

        if (opc == OST_WRITE)
                desc = ptlrpc_prep_bulk_imp(req, oa_bufs, BULK_GET_SOURCE,
                                            OST_BULK_PORTAL);
        else
                desc = ptlrpc_prep_bulk_imp(req, oa_bufs, BULK_PUT_SINK,
                                            OST_BULK_PORTAL);
        LASSERT(desc != NULL);

        body = lustre_msg_buf(req->rq_reqmsg, 0, sizeof(*body));
        ioobj = lustre_msg_buf(req->rq_reqmsg, 1, sizeof(*ioobj));
        niobuf = lustre_msg_buf(req->rq_reqmsg, 2, oa_bufs * sizeof(*niobuf));

        memcpy(&body->oa, oa, sizeof(*oa));
        obdo_to_ioobj(oa, ioobj);
        ioobj->ioo_bufcnt = oa_bufs;

        for (i = 0; i < oa_bufs; i++, niobuf++) {
                struct brw_page *pg = &pgarr[i];

                LASSERT(pg->count > 0);
                LASSERT((pg->off & ~PAGE_MASK) + pg->count <= PAGE_SIZE);

                ptlrpc_prep_bulk_page(desc, pg->pg, pg->off & ~PAGE_MASK,
                                      pg->count);

                niobuf->offset = pg->off;
                niobuf->len = pg->count;
                niobuf->flags = pg->flag;
        }

        /* size[0] still sizeof (*body) */
        if (opc == OST_WRITE) {
                /* 1 RC per niobuf */
                size[1] = sizeof(__u32) * oa_bufs;
                req->rq_replen = lustre_msg_size(2, size);
        } else {
                /* 1 RC for the whole I/O */
                req->rq_replen = lustre_msg_size(1, size);
        }
        err = ptlrpc_queue_wait(req);
        LASSERT(err == 0);

        ptlrpc_req_finished(req);
        return 0;
}

static int mdc_valid_attrs(struct obd_export *exp, struct ll_fid *fid)
{
        struct ldlm_res_id res_id = { .name = {0} };
        struct obd_device *obd = exp->exp_obd;
        struct lustre_handle lockh;
        ldlm_policy_data_t policy;
        int flags;
        ENTRY;

        res_id.name[0] = fid->id;
        res_id.name[1] = fid->generation;
        policy.l_inodebits.bits = MDS_INODELOCK_UPDATE;

        CDEBUG(D_INFO, "trying to match res "LPU64"\n", res_id.name[0]);

        /* FIXME use LDLM_FL_TEST_LOCK instead */
        flags = LDLM_FL_BLOCK_GRANTED | LDLM_FL_CBPENDING;
        if (ldlm_lock_match(obd->obd_namespace, flags, &res_id,
                            LDLM_IBITS, &policy, LCK_PR, &lockh)) {
                ldlm_lock_decref(&lockh, LCK_PR);
                RETURN(1);
        }

        if (ldlm_lock_match(obd->obd_namespace, flags, &res_id,
                            LDLM_IBITS, &policy, LCK_PW, &lockh)) {
                ldlm_lock_decref(&lockh, LCK_PW);
                RETURN(1);
        }
        RETURN(0);
}

struct obd_ops mdc_obd_ops = {
        o_owner:        THIS_MODULE,
        o_attach:       mdc_attach,
        o_detach:       mdc_detach,
        o_setup:        mdc_setup,
        o_precleanup:   mdc_precleanup,
        o_cleanup:      mdc_cleanup,
        o_connect:      client_connect_import,
        o_disconnect:   client_disconnect_export,
        o_iocontrol:    mdc_iocontrol,
        o_set_info:     mdc_set_info,
        o_statfs:       mdc_statfs,
        o_pin:          mdc_pin,
        o_unpin:        mdc_unpin,
        o_llog_init:    mdc_llog_init,
        o_llog_finish:  mdc_llog_finish,
        o_create:       mdc_obj_create,
        o_get_info:     mdc_get_info,
        o_brw:          mdc_brw,
        o_init_ea_size: mdc_init_ea_size,
};

struct md_ops mdc_md_ops = {
        m_getstatus:     mdc_getstatus,
        m_getattr:       mdc_getattr,
        m_change_cbdata: mdc_change_cbdata,
        m_close:         mdc_close,
        m_create:        mdc_create,
        m_done_writing:  mdc_done_writing,
        m_enqueue:       mdc_enqueue,
        m_getattr_name:  mdc_getattr_name,
        m_intent_lock:   mdc_intent_lock,
        m_link:          mdc_link,
        m_rename:        mdc_rename,
        m_setattr:       mdc_setattr,
        m_sync:          mdc_sync,
        m_readpage:      mdc_readpage,
        m_unlink:        mdc_unlink,
        m_valid_attrs:   mdc_valid_attrs,
};

int __init mdc_init(void)
{
        struct lprocfs_static_vars lvars;
        lprocfs_init_vars(mdc, &lvars);
        return class_register_type(&mdc_obd_ops, &mdc_md_ops, lvars.module_vars,
                                   LUSTRE_MDC_NAME);
}

static void /*__exit*/ mdc_exit(void)
{
        class_unregister_type(LUSTRE_MDC_NAME);
}

#ifdef __KERNEL__
MODULE_AUTHOR("Cluster File Systems, Inc. <info@clusterfs.com>");
MODULE_DESCRIPTION("Lustre Metadata Client");
MODULE_LICENSE("GPL");

EXPORT_SYMBOL(mdc_req2lustre_md);
EXPORT_SYMBOL(mdc_change_cbdata);
EXPORT_SYMBOL(mdc_getstatus);
EXPORT_SYMBOL(mdc_getattr);
EXPORT_SYMBOL(mdc_getattr_name);
EXPORT_SYMBOL(mdc_create);
EXPORT_SYMBOL(mdc_unlink);
EXPORT_SYMBOL(mdc_rename);
EXPORT_SYMBOL(mdc_link);
EXPORT_SYMBOL(mdc_readpage);
EXPORT_SYMBOL(mdc_setattr);
EXPORT_SYMBOL(mdc_close);
EXPORT_SYMBOL(mdc_done_writing);
EXPORT_SYMBOL(mdc_sync);
EXPORT_SYMBOL(mdc_set_open_replay_data);
EXPORT_SYMBOL(mdc_clear_open_replay_data);
EXPORT_SYMBOL(mdc_store_inode_generation);

module_init(mdc_init);
module_exit(mdc_exit);
#endif
