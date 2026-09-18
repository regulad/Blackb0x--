//
//  libplist_compat.c
//  Blackb0x
//
//  --stock-securerom's TSS personalization (src/Personalize.cpp)
//  links libtatsu.a (third_party/libtatsu), whose own tss.c genuinely
//  calls a handful of newer libplist convenience functions that this
//  project's historically-pinned libplist (deps::plist,
//  third_party/libplist, ~2.1.0-era -- see libimobiledevice_glue_ext's
//  own CMakeLists.txt comment for why that pin can't just move) doesn't
//  implement at all. Linking a second, current-HEAD copy of libplist
//  (third_party/libplist-modern, already vendored for
//  libimobiledevice_glue_ext/libirecovery_ext's own configure-time
//  version probes) alongside it was tried first and fails outright: both
//  archives' shared core API (plist_new_dict()/plist_free()/etc.) lives
//  in the very same object files as each archive's own newer/older
//  functions, so pulling in the object that provides the missing
//  functions also re-defines the already-linked core API a second time,
//  a real link-time "multiple definition" error with no way to pull one
//  without the other.
//
//  Every function below is ported near-verbatim from
//  third_party/libplist-modern/src/plist.c (current HEAD as of 2026-09),
//  built entirely on primitives deps::plist already has (confirmed
//  present and unchanged in the historical header:
//  plist_dict_get_item()/plist_dict_set_item()/plist_copy()/
//  plist_new_bool()/plist_new_uint()/plist_get_node_type()/
//  plist_get_bool_val()/plist_get_uint_val()/plist_get_string_ptr()/
//  plist_get_data_ptr()/PLIST_IS_STRING()/PLIST_IS_DATA()). Two
//  deliberate deviations from the upstream source, both because the
//  modern-only types/constants they use don't exist in the historical
//  header at all (only their ABI-compatible underlying `int` survives to
//  the actual linked call sites in tss.o, which is all a shim needs):
//    - Return type is plain `int`, not the modern plist_err_t enum.
//    - plist_write_to_stream() only actually needs to support the one
//      way tss.c's own debug_plist() ever calls it (PLIST_FORMAT_XML,
//      PLIST_OPT_NONE) -- reimplemented via the historical plist_to_xml()
//      instead of switching on a `format`/`options` this header can't
//      even name, since nothing in this codebase's actual TSS request
//      path (img3, no SEP/baseband) ever calls it any other way.
//

#include <plist/plist.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int plist_dict_copy_item(plist_t target_dict, plist_t source_dict, const char* key, const char* alt_source_key) {
    plist_t node = plist_dict_get_item(source_dict, alt_source_key ? alt_source_key : key);
    if (!node) return -1;
    plist_dict_set_item(target_dict, key, plist_copy(node));
    return 0;
}

uint8_t plist_dict_get_bool(plist_t dict, const char* key) {
    uint8_t bval = 0;
    uint64_t uintval = 0;
    const char* strval = NULL;
    uint64_t strsz = 0;
    plist_t node = plist_dict_get_item(dict, key);
    if (!node) return 0;
    switch (plist_get_node_type(node)) {
        case PLIST_BOOLEAN:
            plist_get_bool_val(node, &bval);
            break;
        case PLIST_UINT:
            plist_get_uint_val(node, &uintval);
            bval = uintval ? 1 : 0;
            break;
        case PLIST_STRING:
            strval = plist_get_string_ptr(node, NULL);
            if (strval) bval = (strcmp(strval, "true") == 0) ? 1 : 0;
            break;
        case PLIST_DATA:
            strval = (const char*)plist_get_data_ptr(node, &strsz);
            if (strval && strsz == 1) bval = strval[0] ? 1 : 0;
            break;
        default:
            break;
    }
    return bval;
}

int plist_dict_copy_bool(plist_t target_dict, plist_t source_dict, const char* key, const char* alt_source_key) {
    const char* srckey = alt_source_key ? alt_source_key : key;
    if (!plist_dict_get_item(source_dict, srckey)) return -1;
    plist_dict_set_item(target_dict, key, plist_new_bool(plist_dict_get_bool(source_dict, srckey)));
    return 0;
}

uint64_t plist_dict_get_uint(plist_t dict, const char* key) {
    uint64_t uintval = 0;
    const char* strval = NULL;
    uint64_t strsz = 0;
    plist_t node = plist_dict_get_item(dict, key);
    if (!node) return 0;
    switch (plist_get_node_type(node)) {
        case PLIST_UINT:
            plist_get_uint_val(node, &uintval);
            break;
        case PLIST_STRING:
            strval = plist_get_string_ptr(node, NULL);
            if (strval) uintval = strtoull(strval, NULL, 0);
            break;
        case PLIST_DATA:
            strval = (const char*)plist_get_data_ptr(node, &strsz);
            if (strval) {
                if (strsz == 8) memcpy(&uintval, strval, 8);
                else if (strsz == 4) { uint32_t v; memcpy(&v, strval, 4); uintval = v; }
                else if (strsz == 2) { uint16_t v; memcpy(&v, strval, 2); uintval = v; }
                else if (strsz == 1) uintval = (uint8_t)strval[0];
            }
            break;
        default:
            break;
    }
    return uintval;
}

int plist_dict_copy_uint(plist_t target_dict, plist_t source_dict, const char* key, const char* alt_source_key) {
    const char* srckey = alt_source_key ? alt_source_key : key;
    if (!plist_dict_get_item(source_dict, srckey)) return -1;
    plist_dict_set_item(target_dict, key, plist_new_uint(plist_dict_get_uint(source_dict, srckey)));
    return 0;
}

int plist_dict_copy_data(plist_t target_dict, plist_t source_dict, const char* key, const char* alt_source_key) {
    plist_t node = plist_dict_get_item(source_dict, alt_source_key ? alt_source_key : key);
    if (!node || !PLIST_IS_DATA(node)) return -1;
    plist_dict_set_item(target_dict, key, plist_copy(node));
    return 0;
}

int plist_dict_copy_string(plist_t target_dict, plist_t source_dict, const char* key, const char* alt_source_key) {
    plist_t node = plist_dict_get_item(source_dict, alt_source_key ? alt_source_key : key);
    if (!node || !PLIST_IS_STRING(node)) return -1;
    plist_dict_set_item(target_dict, key, plist_copy(node));
    return 0;
}

plist_t plist_new_int(int64_t val) {
    // No signed-integer constructor in the historical header (only
    // plist_new_uint()) -- libplist's PLIST_INT node has always stored a
    // plain 64-bit value with no separate signed/unsigned node subtype,
    // so this is a faithful (not just approximate) equivalent, not a
    // simplification.
    return plist_new_uint((uint64_t)val);
}

int plist_write_to_stream(plist_t plist, FILE* stream, int format, int options) {
    (void)format;
    (void)options;
    char* xml = NULL;
    uint32_t length = 0;
    plist_to_xml(plist, &xml, &length);
    if (!xml) return -1;
    fwrite(xml, 1, length, stream);
    plist_to_xml_free(xml);
    return 0;
}
