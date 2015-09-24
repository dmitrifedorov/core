/* Copyright (c) 2015 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "mail-namespace.h"
#include "mail-storage.h"
#include "mail-storage-private.h"
#include "notify-plugin.h"
#include "str.h"

#include "push-notification-drivers.h"
#include "push-notification-events.h"
#include "push-notification-events-rfc5423.h"
#include "push-notification-plugin.h"
#include "push-notification-triggers.h"
#include "push-notification-txn-mbox.h"
#include "push-notification-txn-msg.h"


#define PUSH_NOTIFICATION_CONFIG "push_notification_driver"
#define PUSH_NOTIFICATION_CONFIG_OLD "push_notification_backend"

#define PUSH_NOTIFICATION_USER_CONTEXT(obj) \
        MODULE_CONTEXT(obj, push_notification_user_module)
static MODULE_CONTEXT_DEFINE_INIT(push_notification_user_module,
                                  &mail_user_module_register);


static struct push_notification_user *puser = NULL;


static struct push_notification_txn *
push_notification_transaction_create(struct mailbox *box,
                                     struct mailbox_transaction_context *t)
{
    struct push_notification_driver_txn *dtxn;
    struct push_notification_driver_user **duser;
    pool_t pool;
    struct push_notification_txn *ptxn;
    struct mail_storage *storage;

    pool = pool_alloconly_create("push notification transaction", 2048);

    ptxn = p_new(pool, struct push_notification_txn, 1);
    ptxn->mbox = box;
    storage = mailbox_get_storage(box);
    ptxn->muser = mail_storage_get_user(storage);
    ptxn->pool = pool;
    ptxn->puser = PUSH_NOTIFICATION_USER_CONTEXT(ptxn->muser);
    ptxn->t = t;
    ptxn->trigger = PUSH_NOTIFICATION_EVENT_TRIGGER_NONE;

    p_array_init(&ptxn->drivers, pool, 4);

    if (storage->user->autocreated &&
        (strcmp(storage->name, "raw") == 0)) {
        /* no notifications for autocreated raw users */
        return ptxn;
    }

    array_foreach_modifiable(&ptxn->puser->drivers, duser) {
        dtxn = p_new(pool, struct push_notification_driver_txn, 1);
        dtxn->duser = *duser;
        dtxn->ptxn = ptxn;

        if ((dtxn->duser->driver->v.begin_txn == NULL) ||
            dtxn->duser->driver->v.begin_txn(dtxn)) {
            array_append(&ptxn->drivers, &dtxn, 1);
        }
    }

    return ptxn;
}

static void push_notification_transaction_end
(struct push_notification_txn *ptxn, bool success)
{
    struct push_notification_driver_txn **dtxn;

    array_foreach_modifiable(&ptxn->drivers, dtxn) {
        if ((*dtxn)->duser->driver->v.end_txn != NULL) {
            (*dtxn)->duser->driver->v.end_txn(*dtxn, success);
        }
    }

    pool_unref(&ptxn->pool);
}

static void push_notification_transaction_commit
(void *txn, struct mail_transaction_commit_changes *changes)
{
    struct push_notification_txn *ptxn = (struct push_notification_txn *)txn;

    if (changes == NULL) {
        push_notification_txn_mbox_end(ptxn);
    } else {
        push_notification_txn_msg_end(ptxn, changes);
    }

    push_notification_transaction_end(ptxn, TRUE);
}

static void push_notification_mailbox_create(struct mailbox *box)
{
    struct push_notification_txn *ptxn;

    ptxn = push_notification_transaction_create(box, NULL);
    push_notification_trigger_mbox_create(ptxn, box, NULL);
    push_notification_transaction_commit(ptxn, NULL);
}

static void push_notification_mailbox_delete(void *txn ATTR_UNUSED,
                                             struct mailbox *box)
{
    struct push_notification_txn *ptxn;

    ptxn = push_notification_transaction_create(box, NULL);
    push_notification_trigger_mbox_delete(ptxn, box, NULL);
    push_notification_transaction_commit(ptxn, NULL);
}

static void push_notification_mailbox_rename(struct mailbox *src,
                                             struct mailbox *dest)
{
    struct push_notification_txn *ptxn;

    ptxn = push_notification_transaction_create(dest, NULL);
    push_notification_trigger_mbox_rename(ptxn, src, dest, NULL);
    push_notification_transaction_commit(ptxn, NULL);
}

static void push_notification_mailbox_subscribe(struct mailbox *box,
                                                bool subscribed)
{
    struct push_notification_txn *ptxn;

    ptxn = push_notification_transaction_create(box, NULL);
    push_notification_trigger_mbox_subscribe(ptxn, box, subscribed, NULL);
    push_notification_transaction_commit(ptxn, NULL);
}

static void push_notification_mail_save(void *txn, struct mail *mail)
{
    struct push_notification_txn *ptxn = (struct push_notification_txn *)txn;

    /* External means a COPY or APPEND IMAP action. */
    if (ptxn->t->flags & MAILBOX_TRANSACTION_FLAG_EXTERNAL) {
        push_notification_trigger_msg_save_append(ptxn, mail, NULL);
    } else {
        push_notification_trigger_msg_save_new(ptxn, mail, NULL);
    }
}

static void push_notification_mail_copy(void *txn,
                                        struct mail *src ATTR_UNUSED,
                                        struct mail *dest)
{
    push_notification_trigger_msg_save_append(
            (struct push_notification_txn *)txn, dest, NULL);
}

static void push_notification_mail_expunge(void *txn, struct mail *mail)
{
    push_notification_trigger_msg_save_expunge(
            (struct push_notification_txn *)txn, mail, NULL);
}

static void
push_notification_mail_update_flags(void *txn, struct mail *mail,
                                    enum mail_flags old_flags)
{
    push_notification_trigger_msg_flag_change(
            (struct push_notification_txn *) txn, mail, NULL, old_flags);
}

static void
push_notification_mail_update_keywords(void *txn, struct mail *mail,
                                       const char *const *old_keywords)
{
    push_notification_trigger_msg_keyword_change(
            (struct push_notification_txn *) txn, mail, NULL, old_keywords);
}

static void *
push_notification_transaction_begin(struct mailbox_transaction_context *t)
{
    return push_notification_transaction_create(mailbox_transaction_get_mailbox(t), t);
}

static void push_notification_transaction_rollback(void *txn)
{
    struct push_notification_txn *ptxn = (struct push_notification_txn *)txn;

    push_notification_transaction_end(ptxn, FALSE);
}

static void
push_notification_user_created_init_config(const char *config_name,
                                           struct mail_user *user,
                                           struct push_notification_user *puser)
{
    struct push_notification_driver_user *duser;
    const char *env;
    unsigned int i;
    string_t *root_name;

    root_name = t_str_new(32);
    str_append(root_name, config_name);

    for (i = 2;; i++) {
        env = mail_user_plugin_getenv(user, str_c(root_name));
        if ((env == NULL) || (*env == '\0')) {
            break;
        }

        if (push_notification_driver_init(user, env, puser->pool, &duser) < 0) {
            break;
        }

        // Add driver.
        array_append(&puser->drivers, &duser, 1);

        str_truncate(root_name, strlen(config_name));
        str_printfa(root_name, "%d", i);
    }
}

static void push_notification_user_created_init(struct mail_user *user)
{
    pool_t pool;

    pool = pool_alloconly_create("push notification plugin", 1024);

    puser = p_new(pool, struct push_notification_user, 1);
    puser->pool = pool;

    p_array_init(&puser->drivers, pool, 4);

    push_notification_user_created_init_config(PUSH_NOTIFICATION_CONFIG, user,
                                               puser);

    if (array_is_empty(&puser->drivers)) {
        /* Support old configuration (it was available at time initial OX
         * driver was first released. */
        push_notification_user_created_init_config(PUSH_NOTIFICATION_CONFIG_OLD,
                                                   user, puser);
    }
}

static void push_notification_user_created(struct mail_user *user)
{
    if (puser == NULL) {
        push_notification_user_created_init(user);
    }

    MODULE_CONTEXT_SET(user, push_notification_user_module, puser);
}


/* Plugin interface. */

const char *push_notification_plugin_version = DOVECOT_ABI_VERSION;
const char *push_notification_plugin_dependencies[] = { "notify", NULL };

extern struct push_notification_driver push_notification_driver_dlog;
extern struct push_notification_driver push_notification_driver_ox;

static struct notify_context *push_notification_ctx;

static const struct notify_vfuncs push_notification_vfuncs = {
    /* Mailbox Events */
    .mailbox_create = push_notification_mailbox_create,
    .mailbox_delete_commit = push_notification_mailbox_delete,
    .mailbox_rename = push_notification_mailbox_rename,
    .mailbox_set_subscribed = push_notification_mailbox_subscribe,

    /* Mail Events */
    .mail_copy = push_notification_mail_copy,
    .mail_save = push_notification_mail_save,
    .mail_expunge = push_notification_mail_expunge,
    .mail_update_flags = push_notification_mail_update_flags,
    .mail_update_keywords = push_notification_mail_update_keywords,
    .mail_transaction_begin = push_notification_transaction_begin,
    .mail_transaction_commit = push_notification_transaction_commit,
    .mail_transaction_rollback = push_notification_transaction_rollback
};

static struct mail_storage_hooks push_notification_storage_hooks = {
    .mail_user_created = push_notification_user_created
};

void push_notification_plugin_init(struct module *module)
{
    push_notification_ctx = notify_register(&push_notification_vfuncs);
    mail_storage_hooks_add(module, &push_notification_storage_hooks);

    push_notification_driver_register(&push_notification_driver_dlog);
    push_notification_driver_register(&push_notification_driver_ox);

    push_notification_event_register_rfc5423_events();
}

void push_notification_plugin_deinit(void)
{
    struct push_notification_driver_user **duser;

    if (puser != NULL) {
        array_foreach_modifiable(&puser->drivers, duser) {
            if ((*duser)->driver->v.deinit != NULL) {
                (*duser)->driver->v.deinit(*duser);
            }

            if ((*duser)->driver->v.cleanup != NULL) {
                (*duser)->driver->v.cleanup();
            }
        }

        array_free(&puser->drivers);
        pool_unref(&puser->pool);
    }

    push_notification_driver_unregister(&push_notification_driver_dlog);
    push_notification_driver_unregister(&push_notification_driver_ox);

    mail_storage_hooks_remove(&push_notification_storage_hooks);
    notify_unregister(push_notification_ctx);
}