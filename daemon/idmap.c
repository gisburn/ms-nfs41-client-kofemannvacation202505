/* NFSv4.1 client for Windows
 * Copyright � 2012 The Regents of the University of Michigan
 *
 * Olga Kornievskaia <aglo@umich.edu>
 * Casey Bodley <cbodley@umich.edu>
 * Roland Mainz <roland.mainz@nrubsig.org>
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * without any warranty; without even the implied warranty of merchantability
 * or fitness for a particular purpose.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 */

#include <Windows.h>
#include <strsafe.h>
#include <Winldap.h>
#include <stdlib.h> /* for strtoul() */
#include <errno.h>

#include "nfs41_build_features.h"
#include "idmap.h"
#include "nfs41_const.h"
#include "list.h"
#include "daemon_debug.h"
#include "util.h"

#define PTR2UID_T(p) ((uid_t)PTR2PTRDIFF_T(p))
#define PTR2GID_T(p) ((gid_t)PTR2PTRDIFF_T(p))
#define PTR2UINT(p)  ((UINT)PTR2PTRDIFF_T(p))
#define UID_T2PTR(u) (PTRDIFF_T2PTR((ptrdiff_t)u))
#define GID_T2PTR(g) (PTRDIFF_T2PTR((ptrdiff_t)g))

#define IDLVL 2         /* dprintf level for idmap logging */
#define CYGWINIDLVL 2   /* dprintf level for idmap logging */

#define FILTER_LEN 1024
#define NAME_LEN 32
#define VAL_LEN 257


enum ldap_class {
    CLASS_USER,
    CLASS_GROUP,

    NUM_CLASSES
};

enum ldap_attr {
    ATTR_USER_NAME,
    ATTR_GROUP_NAME,
    ATTR_PRINCIPAL,
    ATTR_UID,
    ATTR_GID,

    NUM_ATTRIBUTES
};

#define ATTR_FLAG(attr) (1 << (attr))
#define ATTR_ISSET(mask, attr) (((mask) & ATTR_FLAG(attr)) != 0)


/* ldap/cache lookups */
struct idmap_lookup {
    enum ldap_attr attr;
    enum ldap_class klass;
    enum config_type type;
    list_compare_fn compare;
    const void *value;
};


/* configuration */
static const char CONFIG_FILENAME[] = "C:\\etc\\ms-nfs41-idmap.conf";

struct idmap_config {
    /* ldap server information */
    char hostname[NFS41_HOSTNAME_LEN+1];
    char localdomain_name[NFS41_HOSTNAME_LEN+1];
    UINT port;
    UINT version;
    UINT timeout;

    /* ldap schema information */
    char classes[NUM_CLASSES][NAME_LEN];
    char attributes[NUM_ATTRIBUTES][NAME_LEN];
    char base[VAL_LEN];

    /* caching configuration */
    INT cache_ttl;
};


enum config_type {
    TYPE_STR,
    TYPE_INT
};

struct config_option {
    const char *key;
    const char *def;
    enum config_type type;
    size_t offset;
    size_t max_len;
};

/* helper macros for declaring config_options */
#define OPT_INT(key,def,field) \
    { key, def, TYPE_INT, FIELD_OFFSET(struct idmap_config, field), 0 }
#define OPT_STR(key,def,field,len) \
    { key, def, TYPE_STR, FIELD_OFFSET(struct idmap_config, field), len }
#define OPT_CLASS(key,def,index) \
    { key, def, TYPE_STR, FIELD_OFFSET(struct idmap_config, classes[index]), NAME_LEN }
#define OPT_ATTR(key,def,index) \
    { key, def, TYPE_STR, FIELD_OFFSET(struct idmap_config, attributes[index]), NAME_LEN }

/* table of recognized config options, including type and default value */
static const struct config_option g_options[] = {
    /* server information */
    OPT_STR("ldap_hostname", "localhost", hostname, NFS41_HOSTNAME_LEN+1),
    OPT_INT("ldap_port", "389", port),
    OPT_INT("ldap_version", "3", version),
    OPT_INT("ldap_timeout", "0", timeout),

    /* schema information */
    OPT_STR("ldap_base", "cn=localhost", base, VAL_LEN),
    OPT_CLASS("ldap_class_users", "user", CLASS_USER),
    OPT_CLASS("ldap_class_groups", "group", CLASS_GROUP),
    OPT_ATTR("ldap_attr_username", "cn", ATTR_USER_NAME),
    OPT_ATTR("ldap_attr_groupname", "cn", ATTR_GROUP_NAME),
    OPT_ATTR("ldap_attr_gssAuthName", "gssAuthName", ATTR_PRINCIPAL),
    OPT_ATTR("ldap_attr_uidNumber", "uidNumber", ATTR_UID),
    OPT_ATTR("ldap_attr_gidNumber", "gidNumber", ATTR_GID),

    /* caching configuration */
    OPT_INT("cache_ttl", "6000", cache_ttl),
};


/* parse each line into key-value pairs
 * accepts 'key = value' or 'key = "value"',
 * ignores whitespace anywhere outside the ""s */
struct config_pair {
    const char *key, *value;
    size_t key_len, value_len;
};

static int config_parse_pair(
    char *line,
    struct config_pair *pair)
{
    char *pos = line;
    int status = NO_ERROR;

    /* terminate at comment */
    pos = strchr(line, '#');
    if (pos) *pos = 0;

    /* skip whitespace before key */
    pos = line;
    while (isspace(*pos)) pos++;
    pair->key = pos;

    pos = strchr(pos, '=');
    if (pos == NULL) {
        eprintf("missing '='\n");
        status = ERROR_INVALID_PARAMETER;
        goto out;
    }

    /* skip whitespace after key */
    pair->key_len = pos - pair->key;
    while (pair->key_len && isspace(pair->key[pair->key_len-1]))
        pair->key_len--;

    if (pair->key_len <= 0) {
        eprintf("empty key\n");
        status = ERROR_INVALID_PARAMETER;
        goto out;
    }

    /* skip whitespace after = */
    pos++;
    while (isspace(*pos)) pos++;

    if (*pos == 0) {
        eprintf("end of line looking for value\n");
        status = ERROR_INVALID_PARAMETER;
        goto out;
    }

    if (*pos == '\"') {
        /* value is between the "s */
        pair->value = pos + 1;
        pos = strchr(pair->value, '\"');
        if (pos == NULL) {
            eprintf("no matching '\"'\n");
            status = ERROR_INVALID_PARAMETER;
            goto out;
        }
        pair->value_len = pos - pair->value;
    } else {
        pair->value = pos;
        pair->value_len = strlen(pair->value);

        /* skip whitespace after value */
        while (pair->value_len && isspace(pair->value[pair->value_len-1]))
            pair->value_len--;
    }

    /* on success, null terminate the key and value */
    ((char*)pair->key)[pair->key_len] = 0;
    ((char*)pair->value)[pair->value_len] = 0;
out:
    return status;
}

static BOOL parse_uint(
    const char *str,
    UINT *id_out)
{
    PCHAR endp;
    const UINT id = strtoul(str, &endp, 10);

    /* must convert the whole string */
    if ((endp - str) < (ptrdiff_t)strlen(str))
        return FALSE;

    /* result must fit in 32 bits */
    if (id == ULONG_MAX && errno == ERANGE)
        return FALSE;

    *id_out = id;
    return TRUE;
}

/* parse default values from g_options[] into idmap_config */
static int config_defaults(
    struct idmap_config *config)
{
    const struct config_option *option;
    const int count = ARRAYSIZE(g_options);
    char *dst;
    int i, status = NO_ERROR;

    for (i = 0; i < count; i++) {
        option = &g_options[i];
        dst = (char*)config + option->offset;

        if (option->type == TYPE_INT) {
            if (!parse_uint(option->def, (UINT*)dst)) {
                status = ERROR_INVALID_PARAMETER;
                eprintf("failed to parse default value of '%s'=\"%s\": "
                    "expected a number\n", option->key, option->def);
                break;
            }
        } else {
            if (FAILED(StringCchCopyA(dst, option->max_len, option->def))) {
                status = ERROR_BUFFER_OVERFLOW;
                eprintf("failed to parse default value of '%s'=\"%s\": "
                    "buffer overflow > %u\n", option->key, option->def,
                    option->max_len);
                break;
            }
        }
    }
    return status;
}

static int config_find_option(
    const struct config_pair *pair,
    const struct config_option **option)
{
    int i, count = ARRAYSIZE(g_options);
    int status = ERROR_NOT_FOUND;

    /* find the config_option by key */
    for (i = 0; i < count; i++) {
        if (_stricmp(pair->key, g_options[i].key) == 0) {
            *option = &g_options[i];
            status = NO_ERROR;
            break;
        }
    }
    return status;
}

static int config_load(
    struct idmap_config *config,
    const char *filename)
{
    char buffer[1024], *pos;
    FILE *file;
    struct config_pair pair;
    const struct config_option *option;
    int line = 0;
    int status = NO_ERROR;

    /* open the file */
    file = fopen(filename, "r");
    if (file == NULL) {
        eprintf("config_load() failed to open file '%s'\n", filename);
        goto out;
    }

    /* read each line */
    while (fgets(buffer, sizeof(buffer), file)) {
        line++;

        /* skip whitespace */
        pos = buffer;
        while (isspace(*pos)) pos++;

        /* skip comments and empty lines */
        if (*pos == '#' || *pos == 0)
            continue;

        /* parse line into a key=value pair */
        status = config_parse_pair(buffer, &pair);
        if (status) {
            eprintf("error on line %d: '%s'\n", line, buffer);
            break;
        }

        /* find the config_option by key */
        status = config_find_option(&pair, &option);
        if (status) {
            eprintf("unrecognized option '%s' on line %d: '%s'\n",
                pair.key, line, buffer);
            status = ERROR_INVALID_PARAMETER;
            break;
        }

        if (option->type == TYPE_INT) {
            if (!parse_uint(pair.value, (UINT*)((char*)config + option->offset))) {
                status = ERROR_INVALID_PARAMETER;
                eprintf("expected a number on line %d: '%s'=\"%s\"\n",
                    line, pair.key, pair.value);
                break;
            }
        } else {
            if (FAILED(StringCchCopyNA((char*)config + option->offset,
                    option->max_len, pair.value, pair.value_len))) {
                status = ERROR_BUFFER_OVERFLOW;
                eprintf("overflow on line %d: '%s'=\"%s\"\n",
                    line, pair.key, pair.value);
                break;
            }
        }
    }

    fclose(file);
out:
    return status;
}

static int config_init(
    struct idmap_config *config)
{
    int status;

    /* load default values */
    status = config_defaults(config);
    if (status) {
        eprintf("config_defaults() failed with %d\n", status);
        goto out;
    }

    /* load configuration from file */
    status = config_load(config, CONFIG_FILENAME);
    if (status) {
        eprintf("config_load('%s') failed with %d\n", CONFIG_FILENAME, status);
        goto out;
    }
out:
    return status;
}


/* generic cache */
typedef struct list_entry* (*entry_alloc_fn)();
typedef void (*entry_free_fn)(struct list_entry*);
typedef void (*entry_copy_fn)(struct list_entry*, const struct list_entry*);

struct cache_ops {
    entry_alloc_fn entry_alloc;
    entry_free_fn entry_free;
    entry_copy_fn entry_copy;
};

struct idmap_cache {
    struct list_entry head;
    const struct cache_ops *ops;
    SRWLOCK lock;
};


static void cache_init(
    struct idmap_cache *cache,
    const struct cache_ops *ops)
{
    list_init(&cache->head);
    cache->ops = ops;
    InitializeSRWLock(&cache->lock);
}

static void cache_cleanup(
    struct idmap_cache *cache)
{
    struct list_entry *entry, *tmp;
    list_for_each_tmp(entry, tmp, &cache->head)
        cache->ops->entry_free(entry);
    list_init(&cache->head);
}

static int cache_insert(
    struct idmap_cache *cache,
    const struct idmap_lookup *lookup,
    const struct list_entry *src)
{
    struct list_entry *entry;
    int status = NO_ERROR;

    AcquireSRWLockExclusive(&cache->lock);

    /* search for an existing match */
    entry = list_search(&cache->head, lookup->value, lookup->compare);
    if (entry) {
        /* overwrite the existing entry with the new results */
        cache->ops->entry_copy(entry, src);
        goto out;
    }

    /* initialize a new entry and add it to the list */
    entry = cache->ops->entry_alloc();
    if (entry == NULL) {
        status = GetLastError();
        goto out;
    }
    cache->ops->entry_copy(entry, src);
    list_add_head(&cache->head, entry);
out:
    ReleaseSRWLockExclusive(&cache->lock);
    return status;
}

static int cache_lookup(
    struct idmap_cache *cache,
    const struct idmap_lookup *lookup,
    struct list_entry *entry_out)
{
    struct list_entry *entry;
    int status = ERROR_NOT_FOUND;

    AcquireSRWLockShared(&cache->lock);

    entry = list_search(&cache->head, lookup->value, lookup->compare);
    if (entry) {
        /* make a copy for use outside of the lock */
        cache->ops->entry_copy(entry_out, entry);
        status = NO_ERROR;
    }

    ReleaseSRWLockShared(&cache->lock);
    return status;
}


/* user cache */
struct idmap_user {
    struct list_entry entry;
    char username[VAL_LEN];
    char principal[VAL_LEN];
    uid_t uid;
    gid_t gid;
    util_reltimestamp last_updated;
};

static struct list_entry* user_cache_alloc()
{
    struct idmap_user *user = calloc(1, sizeof(struct idmap_user));
    return user == NULL ? NULL : &user->entry;
}
static void user_cache_free(struct list_entry *entry)
{
    free(list_container(entry, struct idmap_user, entry));
}
static void user_cache_copy(
    struct list_entry *lhs,
    const struct list_entry *rhs)
{
    struct idmap_user *dst = list_container(lhs, struct idmap_user, entry);
    const struct idmap_user *src = list_container(rhs, const struct idmap_user, entry);
    StringCchCopyA(dst->username, VAL_LEN, src->username);
    StringCchCopyA(dst->principal, VAL_LEN, src->principal);
    dst->uid = src->uid;
    dst->gid = src->gid;
    dst->last_updated = src->last_updated;
}
static const struct cache_ops user_cache_ops = {
    user_cache_alloc,
    user_cache_free,
    user_cache_copy
};


/* group cache */
struct idmap_group {
    struct list_entry entry;
    char name[VAL_LEN];
    gid_t gid;
    util_reltimestamp last_updated;
};

static struct list_entry* group_cache_alloc()
{
    struct idmap_group *group = calloc(1, sizeof(struct idmap_group));
    return group == NULL ? NULL : &group->entry;
}
static void group_cache_free(struct list_entry *entry)
{
    free(list_container(entry, struct idmap_group, entry));
}
static void group_cache_copy(
    struct list_entry *lhs,
    const struct list_entry *rhs)
{
    struct idmap_group *dst = list_container(lhs, struct idmap_group, entry);
    const struct idmap_group *src = list_container(rhs, const struct idmap_group, entry);
    StringCchCopyA(dst->name, VAL_LEN, src->name);
    dst->gid = src->gid;
    dst->last_updated = src->last_updated;
}
static const struct cache_ops group_cache_ops = {
    group_cache_alloc,
    group_cache_free,
    group_cache_copy
};


/* ldap context */
struct idmap_context {
    struct idmap_config config;
    struct idmap_cache users;
    struct idmap_cache groups;
    LDAP *ldap;
};


static int idmap_filter(
    struct idmap_config *config,
    const struct idmap_lookup *lookup,
    char *filter,
    size_t filter_len)
{
    UINT i;
    int status = NO_ERROR;

    switch (lookup->type) {
    case TYPE_INT:
        i = PTR2UINT(lookup->value);
        if (FAILED(StringCchPrintfA(filter, filter_len,
                "(&(objectClass=%s)(%s=%u))",
                config->classes[lookup->klass],
                config->attributes[lookup->attr], i))) {
            status = ERROR_BUFFER_OVERFLOW;
            eprintf("ldap filter buffer overflow: '%s=%u'\n",
                config->attributes[lookup->attr], i);
        }
        break;

    case TYPE_STR:
        if (FAILED(StringCchPrintfA(filter, filter_len,
                "(&(objectClass=%s)(%s=%s))",
                config->classes[lookup->klass],
                config->attributes[lookup->attr], (const char *)lookup->value))) {
            status = ERROR_BUFFER_OVERFLOW;
            eprintf("ldap filter buffer overflow: '%s=%s'\n",
                config->attributes[lookup->attr], lookup->value);
        }
        break;

    default:
        status = ERROR_INVALID_PARAMETER;
        break;
    }
    return status;
}

static int idmap_query_attrs(
    struct idmap_context *context,
    const struct idmap_lookup *lookup,
    const unsigned attributes,
    const unsigned optional,
    PCHAR *values[],
    const int len)
{
    char filter[FILTER_LEN];
    struct idmap_config *config = &context->config;
    LDAPMessage *res = NULL, *entry;
    int i, status;

    /* format the ldap filter */
    status = idmap_filter(config, lookup, filter, FILTER_LEN);
    if (status)
        goto out;

    /* send the ldap query */
    status = ldap_search_stA(context->ldap, config->base,
        LDAP_SCOPE_SUBTREE, filter, NULL, 0, NULL, &res);
    if (status) {
        eprintf("ldap search for '%s' failed with %d: '%s'\n",
            filter, status, ldap_err2stringA(status));
        status = LdapMapErrorToWin32(status);
        goto out;
    }

    entry = ldap_first_entry(context->ldap, res);
    if (entry == NULL) {
        status = LDAP_NO_RESULTS_RETURNED;
        eprintf("ldap search for '%s' failed with %d: '%s'\n",
            filter, status, ldap_err2stringA(status));
        status = LdapMapErrorToWin32(status);
        goto out;
    }

    /* fetch the attributes */
    for (i = 0; i < len; i++) {
        if (ATTR_ISSET(attributes, i)) {
            values[i] = ldap_get_valuesA(context->ldap,
                entry, config->attributes[i]);

            /* fail if required attributes are missing */
            if (values[i] == NULL && !ATTR_ISSET(optional, i)) {
                status = LDAP_NO_SUCH_ATTRIBUTE;
                eprintf("ldap entry for '%s' missing required "
                    "attribute '%s', returning %d: %s\n",
                    filter, config->attributes[i],
                    status, ldap_err2stringA(status));
                status = LdapMapErrorToWin32(status);
                goto out;
            }
        }
    }
out:
    if (res) ldap_msgfree(res);
    return status;
}

static int idmap_lookup_user(
    struct idmap_context *context,
    const struct idmap_lookup *lookup,
    struct idmap_user *user)
{
    PCHAR* values[NUM_ATTRIBUTES] = { NULL };
#ifndef NFS41_DRIVER_FEATURE_IDMAPPER_CYGWIN
    const unsigned attributes = ATTR_FLAG(ATTR_USER_NAME)
        | ATTR_FLAG(ATTR_PRINCIPAL)
        | ATTR_FLAG(ATTR_UID)
        | ATTR_FLAG(ATTR_GID);
    /* principal is optional; we'll cache it if we have it */
    const unsigned optional = ATTR_FLAG(ATTR_PRINCIPAL);
#endif /* !NFS41_DRIVER_FEATURE_IDMAPPER_CYGWIN */
    int i, status;

    /* check the user cache for an existing entry */
    status = cache_lookup(&context->users, lookup, &user->entry);
    if (status == NO_ERROR) {
        /* don't return expired entries; query new attributes
         * and overwrite the entry with cache_insert() */
        if (UTIL_DIFFRELTIME(UTIL_GETRELTIME(), user->last_updated) < context->config.cache_ttl)
            goto out;
    }
#ifndef NFS41_DRIVER_FEATURE_IDMAPPER_CYGWIN
    /* send the query to the ldap server */
    status = idmap_query_attrs(context, lookup,
        attributes, optional, values, NUM_ATTRIBUTES);
    if (status)
        goto out_free_values;

    /* parse attributes */
    if (FAILED(StringCchCopyA(user->username, VAL_LEN,
            *values[ATTR_USER_NAME]))) {
        eprintf("ldap attribute \"%s\"='%s' longer than %u characters\n",
            context->config.attributes[ATTR_USER_NAME],
            *values[ATTR_USER_NAME], VAL_LEN);
        status = ERROR_BUFFER_OVERFLOW;
        goto out_free_values;
    }
    if (FAILED(StringCchCopyA(user->principal, VAL_LEN,
            values[ATTR_PRINCIPAL] ? *values[ATTR_PRINCIPAL] : ""))) {
        eprintf("ldap attribute \"%s\"='%s' longer than %u characters\n",
            context->config.attributes[ATTR_PRINCIPAL],
            values[ATTR_PRINCIPAL] ? *values[ATTR_PRINCIPAL] : "", VAL_LEN);
        status = ERROR_BUFFER_OVERFLOW;
        goto out_free_values;
    }
    if (!parse_uint(*values[ATTR_UID], &user->uid)) {
        eprintf("failed to parse ldap attribute \"%s\"='%s'\n",
            context->config.attributes[ATTR_UID], *values[ATTR_UID]);
        status = ERROR_INVALID_PARAMETER;
        goto out_free_values;
    }
    if (!parse_uint(*values[ATTR_GID], &user->gid)) {
        eprintf("failed to parse ldap attribute \"%s\"='%s'\n",
            context->config.attributes[ATTR_GID], *values[ATTR_GID]);
        status = ERROR_INVALID_PARAMETER;
        goto out_free_values;
    }
    user->last_updated = UTIL_GETRELTIME();
#else
    if (lookup->attr == ATTR_USER_NAME) {
        char principal_name[VAL_LEN];
        uid_t cy_uid = 0;
        gid_t cy_gid = 0;

        status = ERROR_NOT_FOUND;

        if (!cygwin_getent_passwd(lookup->value, NULL, &cy_uid, &cy_gid)) {
            DPRINTF(CYGWINIDLVL,
                ("# ATTR_USER_NAME: cygwin_getent_passwd: returned '%s', uid=%u, gid=%u\n",
                lookup->value, (unsigned int)cy_uid, (unsigned int)cy_gid));
            (void)snprintf(principal_name, sizeof(principal_name),
                "%s@%s", (const char *)lookup->value,
                context->config.localdomain_name);
            StringCchCopyA(user->username, VAL_LEN, lookup->value);
            StringCchCopyA(user->principal, VAL_LEN, principal_name);
            user->uid = cy_uid;
            user->gid = cy_gid;
            status = 0;
        }
    }
    else if (lookup->attr == ATTR_PRINCIPAL) {
        char search_name[VAL_LEN];
        char principal_name[VAL_LEN];
        char *s;
        uid_t cy_uid = 0;
        gid_t cy_gid = 0;

        status = ERROR_NOT_FOUND;

        /*
         * strip '@' from principal name and use that for getent
         * fixme: This does not work with multiple domains
         */
        (void)strcpy_s(search_name, sizeof(search_name), lookup->value);
        if ((s = strchr(search_name, '@')) != NULL)
            *s = '\0';

        if (!cygwin_getent_passwd(search_name, NULL, &cy_uid, &cy_gid)) {
            DPRINTF(CYGWINIDLVL,
                ("# ATTR_PRINCIPAL: cygwin_getent_passwd: returned '%s', uid=%u, gid=%u\n",
                lookup->value, (unsigned int)cy_uid, (unsigned int)cy_gid));
            (void)snprintf(principal_name, sizeof(principal_name),
                "%s@%s", (const char *)lookup->value,
                context->config.localdomain_name);

            if (!strcmp(principal_name, lookup->value)) {
                StringCchCopyA(user->username, VAL_LEN, search_name);
                StringCchCopyA(user->principal, VAL_LEN, principal_name);
                user->uid = cy_uid;
                user->gid = cy_gid;
                status = 0;
            }
        }
    }
    else if (lookup->attr == ATTR_UID) {
        uid_t search_uid = PTR2UID_T(lookup->value);
        char search_name[VAL_LEN];
        char res_username[VAL_LEN];
        char principal_name[VAL_LEN];
        uid_t cy_uid = 0;
        gid_t cy_gid = 0;

        status = ERROR_NOT_FOUND;

        (void)snprintf(search_name, sizeof(search_name), "%lu", (unsigned long)search_uid);

        if (!cygwin_getent_passwd(search_name, res_username, &cy_uid, &cy_gid)) {
            DPRINTF(CYGWINIDLVL,
                ("# ATTR_UID: cygwin_getent_passwd: returned '%s', uid=%u, gid=%u\n",
                res_username, (unsigned int)cy_uid, (unsigned int)cy_gid));
            (void)snprintf(principal_name, sizeof(principal_name),
                "%s@%s", res_username, context->config.localdomain_name);

            StringCchCopyA(user->username, VAL_LEN, res_username);
            StringCchCopyA(user->principal, VAL_LEN, principal_name);
            user->uid = cy_uid;
            user->gid = cy_gid;
            status = 0;
        }
    }
    else
    {
        status = ERROR_NOT_FOUND;
    }

    if (status == 0) {
        user->last_updated = UTIL_GETRELTIME();
        DPRINTF(CYGWINIDLVL, ("## idmap_lookup_user: "
            "found username='%s', principal='%s', uid=%u, gid=%u\n",
            user->username,
            user->principal,
            (unsigned int)user->uid,
            (unsigned int)user->gid));
    }
#endif /* !NFS41_DRIVER_FEATURE_IDMAPPER_CYGWIN */
    if ((status == 0) && context->config.cache_ttl) {
        /* insert the entry into the cache */
        cache_insert(&context->users, lookup, &user->entry);
    }
#ifndef NFS41_DRIVER_FEATURE_IDMAPPER_CYGWIN
out_free_values:
    for (i = 0; i < NUM_ATTRIBUTES; i++)
        ldap_value_freeA(values[i]);
#endif
out:
    return status;
}

static int idmap_lookup_group(
    struct idmap_context *context,
    const struct idmap_lookup *lookup,
    struct idmap_group *group)
{
    PCHAR* values[NUM_ATTRIBUTES] = { NULL };
#ifndef NFS41_DRIVER_FEATURE_IDMAPPER_CYGWIN
    const unsigned attributes = ATTR_FLAG(ATTR_GROUP_NAME)
        | ATTR_FLAG(ATTR_GID);
#endif
    int i, status;

    /* check the group cache for an existing entry */
    status = cache_lookup(&context->groups, lookup, &group->entry);
    if (status == NO_ERROR) {
        /* don't return expired entries; query new attributes
         * and overwrite the entry with cache_insert() */
        if (UTIL_DIFFRELTIME(UTIL_GETRELTIME(), group->last_updated) < context->config.cache_ttl)
            goto out;
    }
#ifndef NFS41_DRIVER_FEATURE_IDMAPPER_CYGWIN
    /* send the query to the ldap server */
    status = idmap_query_attrs(context, lookup,
        attributes, 0, values, NUM_ATTRIBUTES);
    if (status)
        goto out_free_values;

    /* parse attributes */
    if (FAILED(StringCchCopyA(group->name, VAL_LEN,
            *values[ATTR_GROUP_NAME]))) {
        eprintf("ldap attribute \"%s\"='%s' longer than %u characters\n",
            context->config.attributes[ATTR_GROUP_NAME],
            *values[ATTR_GROUP_NAME], VAL_LEN);
        status = ERROR_BUFFER_OVERFLOW;
        goto out_free_values;
    }
    if (!parse_uint(*values[ATTR_GID], &group->gid)) {
        eprintf("failed to parse ldap attribute \"%s\"='%s'\n",
            context->config.attributes[ATTR_GID], *values[ATTR_GID]);
        status = ERROR_INVALID_PARAMETER;
        goto out_free_values;
    }
    group->last_updated = UTIL_GETRELTIME();
#else
    if (lookup->attr == ATTR_GROUP_NAME) {
        gid_t cy_gid = 0;

        status = ERROR_NOT_FOUND;

        if (!cygwin_getent_group(lookup->value, NULL, &cy_gid)) {
            DPRINTF(CYGWINIDLVL,
                ("# ATTR_GROUP_NAME: cygwin_getent_group: "
                "returned '%s', gid=%u\n",
                lookup->value, (unsigned int)cy_gid));
            StringCchCopyA(group->name, VAL_LEN, lookup->value);
            group->gid = cy_gid;
            status = 0;
        }
    }
    else if (lookup->attr == ATTR_GID) {
        gid_t search_gid = PTR2GID_T(lookup->value);
        char search_name[VAL_LEN];
        char res_groupname[VAL_LEN];
        gid_t cy_gid = 0;

        status = ERROR_NOT_FOUND;

        (void)snprintf(search_name, sizeof(search_name),
            "%lu", (unsigned long)search_gid);

        if (!cygwin_getent_group(search_name, res_groupname, &cy_gid)) {
            DPRINTF(CYGWINIDLVL,
                ("# ATTR_GID: cygwin_getent_group: returned '%s', gid=%u\n",
                res_groupname, (unsigned int)cy_gid));
            StringCchCopyA(group->name, VAL_LEN, res_groupname);
            group->gid = cy_gid;
            status = 0;
        }
    }
    else
    {
        status = ERROR_NOT_FOUND;
    }

    if (status == 0) {
        group->last_updated = UTIL_GETRELTIME();
        DPRINTF(CYGWINIDLVL,
            ("## idmap_lookup_group: found name='%s', gid=%u\n",
            group->name,
            (unsigned int)group->gid));
    }
#endif /* !NFS41_DRIVER_FEATURE_IDMAPPER_CYGWIN */
    if ((status == 0) && context->config.cache_ttl) {
        /* insert the entry into the cache */
        cache_insert(&context->groups, lookup, &group->entry);
    }
#ifndef NFS41_DRIVER_FEATURE_IDMAPPER_CYGWIN
out_free_values:
    for (i = 0; i < NUM_ATTRIBUTES; i++)
        ldap_value_freeA(values[i]);
#endif
out:
    return status;
}

/* public idmap interface */
int nfs41_idmap_create(
    struct idmap_context **context_out, const char *localdomain_name)
{
    struct idmap_context *context;
    int status = NO_ERROR;

    context = calloc(1, sizeof(struct idmap_context));
    if (context == NULL) {
        status = GetLastError();
        goto out;
    }

    (void)strcpy_s(context->config.localdomain_name,
        sizeof(context->config.localdomain_name),
        localdomain_name);
    if (context == NULL) {
        status = GetLastError();
        goto out;
    }

    /* initialize the caches */
    cache_init(&context->users, &user_cache_ops);
    cache_init(&context->groups, &group_cache_ops);

    /* load ldap configuration from file */
    status = config_init(&context->config);
    if (status) {
        eprintf("config_init() failed with %d\n", status);
        goto out_err_free;
    }

#ifndef NFS41_DRIVER_FEATURE_IDMAPPER_CYGWIN
    /* initialize ldap and configure options */
    context->ldap = ldap_init(context->config.hostname, context->config.port);
    if (context->ldap == NULL) {
        status = LdapGetLastError();
        eprintf("ldap_init(%s) failed with %d: '%s'\n",
            context->config.hostname, status, ldap_err2stringA(status));
        status = LdapMapErrorToWin32(status);
        goto out_err_free;
    }

    status = ldap_set_option(context->ldap, LDAP_OPT_PROTOCOL_VERSION,
        (void *)&context->config.version);
    if (status != LDAP_SUCCESS) {
        eprintf("ldap_set_option(version=%d) failed with %d\n",
            context->config.version, status);
        status = LdapMapErrorToWin32(status);
        goto out_err_free;
    }

    if (context->config.timeout) {
        status = ldap_set_option(context->ldap, LDAP_OPT_TIMELIMIT,
            (void *)&context->config.timeout);
        if (status != LDAP_SUCCESS) {
            eprintf("ldap_set_option(timeout=%d) failed with %d\n",
                context->config.timeout, status);
            status = LdapMapErrorToWin32(status);
            goto out_err_free;
        }
    }
#else
    DPRINTF(CYGWINIDLVL, ("nfs41_idmap_create: Force context->config.timeout = 6000;\n"));
    context->config.timeout = 6000;
#endif

    *context_out = context;

out:
    return status;

out_err_free:
    nfs41_idmap_free(context);
    goto out;
}

void nfs41_idmap_free(
    struct idmap_context *context)
{
    /* clean up the connection */
    if (context->ldap)
        ldap_unbind(context->ldap);

    cache_cleanup(&context->users);
    cache_cleanup(&context->groups);
    free(context);
}


/* username -> uid, gid */
static int username_cmp(const struct list_entry *list, const void *value)
{
    const struct idmap_user *entry = list_container(list,
        const struct idmap_user, entry);
    const char *username = (const char*)value;
    return strcmp(entry->username, username);
}


int nfs41_idmap_name_to_uid(
    struct idmap_context *context,
    const char *username,
    uid_t *uid_out)
{
    struct idmap_lookup lookup = {
        .attr = ATTR_USER_NAME,
        .klass = CLASS_USER,
        .type = TYPE_STR,
        .compare = username_cmp,
        .value = NULL
    };
    struct idmap_user user;
    int status;

    DPRINTF(IDLVL, ("--> nfs41_idmap_name_to_uid('%s')\n", username));

    lookup.value = username;

    /* look up the user entry */
    status = idmap_lookup_user(context, &lookup, &user);
    if (status) {
        DPRINTF(IDLVL, ("<-- nfs41_idmap_name_to_uid('%s') "
            "failed with %d\n", username, status));
        goto out;
    }

    *uid_out = user.uid;
    DPRINTF(IDLVL, ("<-- nfs41_idmap_name_to_uid('%s') "
        "returning uid=%u\n", username, user.uid));
out:
    return status;
}

int nfs41_idmap_name_to_ids(
    struct idmap_context *context,
    const char *username,
    uid_t *uid_out,
    gid_t *gid_out)
{
    struct idmap_lookup lookup = {
        .attr = ATTR_USER_NAME,
        .klass = CLASS_USER,
        .type = TYPE_STR,
        .compare = username_cmp,
        .value = NULL
    };
    struct idmap_user user;
    int status;

    if (context == NULL)
        return ERROR_FILE_NOT_FOUND;

    DPRINTF(IDLVL, ("--> nfs41_idmap_name_to_ids('%s')\n", username));

    lookup.value = username;

    /* look up the user entry */
    status = idmap_lookup_user(context, &lookup, &user);
    if (status) {
        DPRINTF(IDLVL, ("<-- nfs41_idmap_name_to_ids('%s') "
            "failed with %d\n", username, status));
        goto out;
    }

    *uid_out = user.uid;
    *gid_out = user.gid;
    DPRINTF(IDLVL, ("<-- nfs41_idmap_name_to_ids('%s') "
        "returning uid=%u, gid=%u\n", username, user.uid, user.gid));
out:
    return status;
}

/* uid -> username */
static int uid_cmp(const struct list_entry *list, const void *value)
{
    const struct idmap_user *entry = list_container(list,
        const struct idmap_user, entry);
    const uid_t uid = PTR2UID_T(value);
    return (int)uid - (int)entry->uid;
}

int nfs41_idmap_uid_to_name(
    struct idmap_context *context,
    uid_t uid,
    char *name,
    size_t len)
{
    struct idmap_lookup lookup = {
        .attr = ATTR_UID,
        .klass = CLASS_USER,
        .type = TYPE_INT,
        .compare = uid_cmp,
        .value = NULL
    };
    struct idmap_user user;
    int status;

    DPRINTF(IDLVL, ("--> nfs41_idmap_uid_to_name(%u)\n", (unsigned int)uid));

    lookup.value = UID_T2PTR(uid);

    /* look up the user entry */
    status = idmap_lookup_user(context, &lookup, &user);
    if (status) {
        DPRINTF(IDLVL, ("<-- nfs41_idmap_uid_to_name(%u) "
            "failed with %d\n", (unsigned int)uid, status));
        goto out;
    }

    if (FAILED(StringCchCopyA(name, len, user.username))) {
        status = ERROR_BUFFER_OVERFLOW;
        eprintf("username buffer overflow: '%s' > %u\n",
            user.username, len);
        goto out;
    }

    DPRINTF(IDLVL, ("<-- nfs41_idmap_uid_to_name(%u) "
        "returning '%s'\n", uid, name));
out:
    return status;
}

/* principal -> uid, gid */
static int principal_cmp(const struct list_entry *list, const void *value)
{
    const struct idmap_user *entry = list_container(list,
        const struct idmap_user, entry);
    const char *principal = (const char*)value;
    return strcmp(entry->principal, principal);
}

int nfs41_idmap_principal_to_ids(
    struct idmap_context *context,
    const char *principal,
    uid_t *uid_out,
    gid_t *gid_out)
{
    struct idmap_lookup lookup = {
        .attr = ATTR_PRINCIPAL,
        .klass = CLASS_USER,
        .type = TYPE_STR,
        .compare = principal_cmp,
        .value = NULL
    };
    struct idmap_user user;
    int status;

    DPRINTF(IDLVL, ("--> nfs41_idmap_principal_to_ids('%s')\n", principal));

    lookup.value = principal;

    /* look up the user entry */
    status = idmap_lookup_user(context, &lookup, &user);
    if (status) {
        DPRINTF(IDLVL, ("<-- nfs41_idmap_principal_to_ids('%s') "
            "failed with %d\n", principal, status));
        goto out;
    }

    *uid_out = user.uid;
    *gid_out = user.gid;
    DPRINTF(IDLVL, ("<-- nfs41_idmap_principal_to_ids('%s') "
        "returning uid=%u, gid=%u\n", principal, user.uid, user.gid));
out:
    return status;
}

/* group -> gid */
static int group_cmp(const struct list_entry *list, const void *value)
{
    const struct idmap_group *entry = list_container(list,
        const struct idmap_group, entry);
    const char *group = (const char*)value;
    return strcmp(entry->name, group);
}

int nfs41_idmap_group_to_gid(
    struct idmap_context *context,
    const char *name,
    gid_t *gid_out)
{
    struct idmap_lookup lookup = {
        .attr = ATTR_GROUP_NAME,
        .klass = CLASS_GROUP,
        .type = TYPE_STR,
        .compare = group_cmp,
        .value = NULL
    };
    struct idmap_group group;
    int status;

    DPRINTF(IDLVL, ("--> nfs41_idmap_group_to_gid('%s')\n", name));

    lookup.value = name;

    /* look up the group entry */
    status = idmap_lookup_group(context, &lookup, &group);
    if (status) {
        DPRINTF(IDLVL, ("<-- nfs41_idmap_group_to_gid('%s') "
            "failed with %d\n", name, status));
        goto out;
    }

    *gid_out = group.gid;
    DPRINTF(IDLVL, ("<-- nfs41_idmap_group_to_gid('%s') "
        "returning %u\n", name, group.gid));
out:
    return status;
}

/* gid -> group */
static int gid_cmp(const struct list_entry *list, const void *value)
{
    const struct idmap_group *entry = list_container(list,
        const struct idmap_group, entry);
    const gid_t gid = PTR2GID_T(value);
    return (int)gid - (int)entry->gid;
}

int nfs41_idmap_gid_to_group(
    struct idmap_context *context,
    gid_t gid,
    char *name,
    size_t len)
{
    struct idmap_lookup lookup = {
        .attr = ATTR_GID,
        .klass = CLASS_GROUP,
        .type = TYPE_INT,
        .compare = gid_cmp,
        .value = NULL
    };
    struct idmap_group group;
    int status;

    DPRINTF(IDLVL, ("--> nfs41_idmap_gid_to_group(%u)\n", gid));

    lookup.value = GID_T2PTR(gid);

    /* look up the group entry */
    status = idmap_lookup_group(context, &lookup, &group);
    if (status) {
        DPRINTF(IDLVL, ("<-- nfs41_idmap_gid_to_group(%u) "
            "failed with %d\n", gid, status));
        goto out;
    }

    if (FAILED(StringCchCopyA(name, len, group.name))) {
        status = ERROR_BUFFER_OVERFLOW;
        eprintf("group name buffer overflow: '%s' > %u\n",
            group.name, len);
        goto out;
    }

    DPRINTF(IDLVL, ("<-- nfs41_idmap_gid_to_group(%u) "
        "returning '%s'\n", gid, name));
out:
    return status;
}
