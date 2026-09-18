/*
 * Apple Device Tree.
 *
 * Copyright (c) 2024-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "hw/arm/dt.h"
#include "hw/qdev-core.h"
#include "qapi/error.h"
#include "qemu/cutils.h"

#if 0
    #include "qemu/error-report.h"
    #define DINFO(fmt, ...) info_report(fmt, ##__VA_ARGS__)
    #define DWARN(fmt, ...) warn_report(fmt, ##__VA_ARGS__)
#else
    #define DINFO(fmt, ...) \
        do { }              \
        while (0)
    #define DWARN(fmt, ...) \
        do { }              \
        while (0)
#endif

#define APPLE_DT_PROP_NAME_LEN    (32)
#define APPLE_DT_PROP_PLACEHOLDER (1 << 31)

static void apple_dt_prop_destroy(gpointer data)
{
    AppleDTProp* prop;

    prop = data;

    g_free(prop->data);
    g_free(prop);
}

static AppleDTNode* apple_dt_new_node(void)
{
    AppleDTNode* node;

    node = g_new0(AppleDTNode, 1);

    node->props = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, apple_dt_prop_destroy);

    return node;
}

static void apple_dt_destroy_node(AppleDTNode* node)
{
    g_hash_table_unref(node->props);

    if (node->children != NULL) { g_list_free_full(node->children, (GDestroyNotify)apple_dt_destroy_node); }

    g_free(node);
}

AppleDTNode* apple_dt_node_new(AppleDTNode* parent, const char* name)
{
    AppleDTNode* node;

    // Only the root node can have no name.
    if (parent != NULL) {
        assert_false(parent->finalised);
        if (name == NULL || apple_dt_get_node(parent, name) != NULL) { return NULL; }
    }

    node = apple_dt_new_node();

    if (name != NULL) { apple_dt_set_prop_str(node, "name", name); }

    if (parent != NULL) { parent->children = g_list_append(parent->children, node); }

    return node;
}

static AppleDTProp* apple_dt_deserialise_prop(void** blob, char** name)
{
    AppleDTProp* prop;
    size_t       name_len;

    prop     = g_new0(AppleDTProp, 1);
    name_len = strnlen(*blob, APPLE_DT_PROP_NAME_LEN - 1);
    *name    = g_new(char, name_len + 1);
    memcpy(*name, *blob, name_len);
    (*name)[name_len]  = '\0';
    *blob             += APPLE_DT_PROP_NAME_LEN;

    prop->len = ldl_le_p(*blob);
    if (prop->len & APPLE_DT_PROP_PLACEHOLDER) {
        prop->placeholder  = true;
        prop->len         &= ~APPLE_DT_PROP_PLACEHOLDER;
    }
    *blob += sizeof(uint32_t);

    if (prop->len != 0) {
        prop->data = g_malloc0(prop->len);
        memcpy(prop->data, *blob, prop->len);
        *blob += ROUND_UP(prop->len, 4);
    }

    return prop;
}

static AppleDTNode* apple_dt_deserialise_node(void** blob)
{
    uint32_t     i;
    AppleDTNode* node;
    uint32_t     prop_count;
    uint32_t     children_count;
    AppleDTNode* child;
    AppleDTProp* prop;
    char*        key;

    if (blob == NULL || *blob == NULL) { return NULL; }

    node            = apple_dt_new_node();
    prop_count      = ldl_le_p(*blob);
    *blob          += sizeof(prop_count);
    children_count  = ldl_le_p(*blob);
    *blob          += sizeof(children_count);

    for (i = 0; i < prop_count; i++) {
        prop = apple_dt_deserialise_prop(blob, &key);
        if (prop == NULL) {
            apple_dt_destroy_node(node);
            return NULL;
        }
        g_hash_table_insert(node->props, key, prop);
    }

    for (i = 0; i < children_count; i++) {
        child = apple_dt_deserialise_node(blob);
        if (child == NULL) {
            apple_dt_destroy_node(node);
            return NULL;
        }
        node->children = g_list_append(node->children, child);
    }

    return node;
}

AppleDTNode* apple_dt_deserialise(void* blob) { return apple_dt_deserialise_node(&blob); }

void apple_dt_del_node(AppleDTNode* parent, AppleDTNode* node)
{
    assert_false(parent == node);
    assert_false(parent->finalised);
    assert_false(node->finalised);

    parent->children = g_list_remove(parent->children, node);
    apple_dt_destroy_node(node);
}

bool apple_dt_del_node_named(AppleDTNode* parent, const char* name)
{
    AppleDTNode* node;

    node = apple_dt_get_node(parent, name);

    if (node == NULL) { return false; }

    apple_dt_del_node(parent, node);
    return true;
}

bool apple_dt_del_prop_named(AppleDTNode* node, const char* name) { return g_hash_table_remove(node->props, name); }

AppleDTProp* apple_dt_set_prop(AppleDTNode* node, const char* name, const uint32_t len, const void* val)
{
    AppleDTProp* prop;

    assert_cmpint(strlen(name), <, APPLE_DT_PROP_NAME_LEN);

    prop = apple_dt_get_prop(node, name);

    if (prop == NULL) {
        assert_false(node->finalised);

        prop = g_new0(AppleDTProp, 1);
        g_hash_table_insert(node->props, g_strdup(name), prop);
    }
    else {
        assert_false(node->finalised && prop->len != len);

        g_free(prop->data);
        *prop = (AppleDTProp){0};
    }

    if (val == NULL) { prop->data = g_malloc0(len); }
    else {
        prop->data = g_malloc(len);
        memcpy(prop->data, val, len);
    }
    prop->len = len;

    return prop;
}

AppleDTProp* apple_dt_set_prop_null(AppleDTNode* node, const char* name)
{ return apple_dt_set_prop(node, name, 0, NULL); }

AppleDTProp* apple_dt_set_prop_u32(AppleDTNode* node, const char* name, uint32_t val)
{
    val = cpu_to_le32(val);
    return apple_dt_set_prop(node, name, sizeof(val), &val);
}

AppleDTProp* apple_dt_set_prop_u64(AppleDTNode* node, const char* name, uint64_t val)
{
    val = cpu_to_le64(val);
    return apple_dt_set_prop(node, name, sizeof(val), &val);
}

AppleDTProp* apple_dt_set_prop_str(AppleDTNode* node, const char* name, const char* val)
{ return apple_dt_set_prop(node, name, strlen(val) + 1, val); }

AppleDTProp* apple_dt_set_prop_strn(AppleDTNode* node, const char* name, uint32_t max_len, const char* val)
{
    g_autofree char* buf;

    buf = g_malloc(max_len);
    strncpy(buf, val, max_len);
    return apple_dt_set_prop(node, name, max_len, buf);
}

AppleDTProp* apple_dt_get_prop(AppleDTNode* node, const char* name) { return g_hash_table_lookup(node->props, name); }

static const char* apple_dt_get_prop_str_full(AppleDTNode* node, const char* name, const char* default_s,
                                              Error** missing_errp, Error** invalid_errp)
{
    AppleDTProp* prop = apple_dt_get_prop(node, name);
    if (prop == NULL) {
        error_setg(missing_errp, "`%s` is missing", name);
        return default_s;
    }
    if (prop->len == 0 || ((char*)prop->data)[prop->len - 1] != '\0') {
        error_setg(invalid_errp, "`%s` is not a valid C string", name);
        return default_s;
    }
    return prop->data;
}

static char* apple_dt_get_prop_strdup_full(AppleDTNode* node, const char* name, const char* default_s,
                                           Error** missing_errp, Error** invalid_errp)
{ return g_strdup(apple_dt_get_prop_str_full(node, name, default_s, missing_errp, invalid_errp)); }

static Error** apple_dt_missing_errp_from_errp(Error** errp)
{ return errp == NULL || errp == &error_warn ? &error_fatal : errp; }

const char* apple_dt_get_prop_str(AppleDTNode* node, const char* name, Error** errp)
{ return apple_dt_get_prop_str_full(node, name, NULL, apple_dt_missing_errp_from_errp(errp), errp); }

char* apple_dt_get_prop_strdup(AppleDTNode* node, const char* name, Error** errp)
{ return apple_dt_get_prop_strdup_full(node, name, NULL, apple_dt_missing_errp_from_errp(errp), errp); }

const char* apple_dt_get_prop_str_or(AppleDTNode* node, const char* name, const char* default_s, Error** errp)
{ return apple_dt_get_prop_str_full(node, name, default_s, NULL, errp); }

char* apple_dt_get_prop_strdup_or(AppleDTNode* node, const char* name, const char* default_s, Error** errp)
{ return apple_dt_get_prop_strdup_full(node, name, default_s, NULL, errp); }

#define DEFINE_GET_PROP_INT(_sign, _width, _accessor)                                                                  \
    static _sign##int##_width##_t apple_dt_get_prop_##_sign##_width##_full(AppleDTNode* node, const char* name,        \
                                                                           _sign##int##_width##_t default_val,         \
                                                                           Error** missing_errp, Error** invalid_errp) \
    {                                                                                                                  \
        AppleDTProp* prop = apple_dt_get_prop(node, name);                                                             \
        if (prop == NULL) {                                                                                            \
            error_setg(missing_errp, "`%s` is missing", name);                                                         \
            return default_val;                                                                                        \
        }                                                                                                              \
        if (prop->len != sizeof(default_val)) {                                                                        \
            if (prop->len < sizeof(default_val)) {                                                                     \
                error_setg(&error_fatal, "`%s` len mismatch; wanted: %zu, actual: %u", name, sizeof(default_val),      \
                           prop->len);                                                                                 \
            }                                                                                                          \
            else {                                                                                                     \
                error_setg(invalid_errp, "`%s` len mismatch; wanted: %zu, actual: %u", name, sizeof(default_val),      \
                           prop->len);                                                                                 \
            }                                                                                                          \
        }                                                                                                              \
        return _accessor(prop->data);                                                                                  \
    }

#define DEFINE_GET_PROP_LD(_sign, _width, _ldsuffix) DEFINE_GET_PROP_INT(_sign, _width, ld##_ldsuffix##_le_p)

DEFINE_GET_PROP_INT(u, 8, *(uint8_t*));
DEFINE_GET_PROP_LD(u, 16, uw);
DEFINE_GET_PROP_LD(u, 32, l);
DEFINE_GET_PROP_LD(u, 64, q);

uint8_t apple_dt_get_prop_u8_or(AppleDTNode* node, const char* name, uint8_t default_val, Error** invalid_errp)
{ return apple_dt_get_prop_u8_full(node, name, default_val, NULL, invalid_errp); }

uint16_t apple_dt_get_prop_u16_or(AppleDTNode* node, const char* name, uint16_t default_val, Error** invalid_errp)
{ return apple_dt_get_prop_u16_full(node, name, default_val, NULL, invalid_errp); }

uint32_t apple_dt_get_prop_u32_or(AppleDTNode* node, const char* name, uint32_t default_val, Error** invalid_errp)
{ return apple_dt_get_prop_u32_full(node, name, default_val, NULL, invalid_errp); }

uint64_t apple_dt_get_prop_u64_or(AppleDTNode* node, const char* name, uint64_t default_val, Error** invalid_errp)
{ return apple_dt_get_prop_u64_full(node, name, default_val, NULL, invalid_errp); }

uint8_t apple_dt_get_prop_u8(AppleDTNode* node, const char* name, Error** errp)
{ return apple_dt_get_prop_u8_full(node, name, 0, apple_dt_missing_errp_from_errp(errp), errp); }

uint16_t apple_dt_get_prop_u16(AppleDTNode* node, const char* name, Error** errp)
{ return apple_dt_get_prop_u16_full(node, name, 0, apple_dt_missing_errp_from_errp(errp), errp); }

uint32_t apple_dt_get_prop_u32(AppleDTNode* node, const char* name, Error** errp)
{ return apple_dt_get_prop_u32_full(node, name, 0, apple_dt_missing_errp_from_errp(errp), errp); }

uint64_t apple_dt_get_prop_u64(AppleDTNode* node, const char* name, Error** errp)
{ return apple_dt_get_prop_u64_full(node, name, 0, apple_dt_missing_errp_from_errp(errp), errp); }

AppleDTNode* apple_dt_get_node(AppleDTNode* node, const char* path)
{
    GList*       iter;
    AppleDTProp* prop;
    AppleDTNode* child;
    char*        next;
    char*        string;
    const char*  token;
    bool         found;

    next = string = g_strdup(path);

    while (node != NULL && ((token = qemu_strsep(&next, "/")))) {
        if (*token == '\0') { continue; }

        found = false;

        for (iter = node->children; iter; iter = iter->next) {
            child = (AppleDTNode*)iter->data;

            prop = apple_dt_get_prop(child, "name");

            if (prop == NULL) { continue; }

            if (strncmp((const char*)prop->data, token, prop->len) == 0) {
                node  = child;
                found = true;
                break;
            }
        }

        if (!found) {
            g_free(string);
            return NULL;
        }
    }

    g_free(string);
    return node;
}

void apple_dt_connect_function_prop_out_in(DeviceState* target_device, DeviceState* src_device,
                                           AppleDTProp* function_prop, const char* name)
{
    uint32_t* ints;
    uint32_t  pin;

    assert_nonnull(function_prop);

    ints = (uint32_t*)function_prop->data;
    pin  = ints[2];
    qdev_connect_gpio_out(target_device, pin, qdev_get_gpio_in_named(src_device, name, 0));
}

void apple_dt_connect_function_prop_out_in_gpio(DeviceState* src_device, AppleDTProp* function_prop,
                                                const char* gpio_name)
{
    DeviceState* gpio;

    gpio = DEVICE(object_property_get_link(OBJECT(qdev_get_machine()), "gpio", &error_fatal));
    apple_dt_connect_function_prop_out_in(gpio, src_device, function_prop, gpio_name);
}

void apple_dt_connect_function_prop_in_out(DeviceState* target_device, DeviceState* src_device,
                                           AppleDTProp* function_prop, const char* name)
{
    uint32_t* ints;
    uint32_t  pin;

    assert_nonnull(function_prop);

    ints = (uint32_t*)function_prop->data;
    pin  = ints[2];
    qdev_connect_gpio_out_named(src_device, name, 0, qdev_get_gpio_in(target_device, pin));
}

void apple_dt_connect_function_prop_in_out_gpio(DeviceState* src_device, AppleDTProp* function_prop,
                                                const char* gpio_name)
{
    DeviceState* gpio;

    gpio = DEVICE(object_property_get_link(OBJECT(qdev_get_machine()), "gpio", &error_fatal));
    apple_dt_connect_function_prop_in_out(gpio, src_device, function_prop, gpio_name);
}

static uint32_t apple_dt_prop_placeholder_len(AppleDTProp* prop)
{
    char*       string;
    char*       next;
    const char* token;
    uint32_t    len;

    if (prop->len == 0) { return 0; }

    next = string = g_new0(char, prop->len);
    memcpy(next, prop->data, prop->len);

    while ((token = qemu_strsep(&next, ",")) != NULL) {
        if (*token == '\0') { continue; }

        if (strncmp(token, "macaddr/", 8) == 0) {
            g_free(string);
            return 6;
        }

        if (strncmp(token, "syscfg/", 7) == 0) {
            if (strlen(token) < 12 || token[11] != '/') { continue; }
            len = g_ascii_strtoull(token + 8 + 4, NULL, 0);
            if (len == 0) { continue; }
            g_free(string);
            return len;
        }

        if (strncmp(token, "zeroes/", 7) == 0) {
            len = g_ascii_strtoull(token + 7, NULL, 0);
            if (len == 0) { continue; }
            g_free(string);
            return len;
        }
    }

    g_free(string);
    return 0;
}

static void apple_dt_serialise_node(AppleDTNode* node, void** buf)
{
    void*          prop_count_buf;
    uint32_t       prop_count;
    GHashTableIter prop_iter;
    gpointer       key;
    AppleDTProp*   prop;
    uint32_t       placeholder_len;
    uint32_t       padded;

    assert_true(node->finalised);

    prop_count_buf  = *buf;
    *buf           += sizeof(uint32_t);

    stl_le_p(*buf, g_list_length(node->children));
    *buf += sizeof(uint32_t);

    prop_count = 0;
    g_hash_table_iter_init(&prop_iter, node->props);
    while (g_hash_table_iter_next(&prop_iter, &key, (gpointer*)&prop)) {
        // TODO: put a system to register things like syscfg values.
        // who's going to have to do it? spoiler: it will be me, Visual, once
        // again.
        if (prop->placeholder) {
            placeholder_len = apple_dt_prop_placeholder_len(prop);
            if (placeholder_len == 0) {
                DWARN("Removing prop `%s`", (char*)key);
                continue;
            }
            DWARN("Expanding prop `%s` to default value", (char*)key);
            strncpy(*buf, key, APPLE_DT_PROP_NAME_LEN);
            *buf += APPLE_DT_PROP_NAME_LEN;
            stl_le_p(*buf, placeholder_len);
            *buf   += sizeof(uint32_t);
            padded  = ROUND_UP(placeholder_len, 4);
            memset(*buf, 0, padded);
            *buf += padded;
        }
        else {
            strncpy(*buf, key, APPLE_DT_PROP_NAME_LEN);
            *buf += APPLE_DT_PROP_NAME_LEN;
            stl_le_p(*buf, prop->len);
            *buf += sizeof(uint32_t);

            if (prop->len != 0) {
                padded = ROUND_UP(prop->len, 4);
                memcpy(*buf, prop->data, prop->len);
                memset(*buf + prop->len, 0, padded - prop->len);
                *buf += padded;
            }
        }
        prop_count += 1;
    }
    stl_le_p(prop_count_buf, prop_count);

    g_list_foreach(node->children, (GFunc)apple_dt_serialise_node, buf);
}

void apple_dt_serialise(AppleDTNode* root, void* buf) { apple_dt_serialise_node(root, &buf); }

static uint64_t apple_dt_prop_serialised_len(AppleDTProp* prop)
{
    uint32_t placeholder_len;

    if (prop->placeholder) {
        placeholder_len = apple_dt_prop_placeholder_len(prop);
        if (placeholder_len == 0) { return 0; }
        return APPLE_DT_PROP_NAME_LEN + sizeof(prop->len) + ROUND_UP(placeholder_len, 4);
    }

    return APPLE_DT_PROP_NAME_LEN + sizeof(prop->len) + ROUND_UP(prop->len, 4);
}

uint64_t apple_dt_finalise(AppleDTNode* node)
{
    GHashTableIter prop_iter;
    gpointer       key;
    AppleDTProp*   prop;
    uint64_t       len;
    GList*         child_iter;

    assert_false(node->finalised);

    node->finalised = true;

    len = sizeof(uint32_t) + sizeof(uint32_t);

    g_hash_table_iter_init(&prop_iter, node->props);
    while (g_hash_table_iter_next(&prop_iter, &key, (gpointer*)&prop)) { len += apple_dt_prop_serialised_len(prop); }

    for (child_iter = node->children; child_iter != NULL; child_iter = child_iter->next) {
        len += apple_dt_finalise(child_iter->data);
    }

    return len;
}

void apple_dt_unfinalise(AppleDTNode* root)
{
    root->finalised = false;

    g_list_foreach(root->children, (GFunc)apple_dt_unfinalise, NULL);
}
