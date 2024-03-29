#include "hz.h"
#include "util/hz-array.h"
#include "util/hz-map.h"


void
hz_context_set_features(hz_context_t *ctx, hz_array_t *features)
{
    ctx->features = features;
}

hz_context_t *
hz_context_create(hz_font_t *font)
{
    hz_context_t *ctx = HZ_ALLOC(hz_context_t);

    ctx->font = font;
//    ctx->features = hz_array_create();

    return ctx;
}

void
hz_context_set_script(hz_context_t *ctx, hz_script_t script)
{
    ctx->script = script;
}

void
hz_context_set_language(hz_context_t *ctx, hz_language_t language)
{
    ctx->language = language;
}

void
hz_context_set_direction(hz_context_t *ctx, hz_direction_t dir)
{
    ctx->dir = dir;
}

void
hz_context_destroy(hz_context_t *ctx)
{
    free(ctx);
}

/* https://docs.microsoft.com/en-us/typography/opentype/spec/cmap#platform-ids */
typedef enum hz_cmap_platform_t  {
    HZ_CMAP_PLATFORM_UNICODE = 0, /* Various */
    HZ_CMAP_PLATFORM_MACINTOSH = 1, /* Script manager code */
    HZ_CMAP_PLATFORM_ISO = 2, /* ISO encoding [deprecated] */
    HZ_CMAP_PLATFORM_WINDOWS = 3, /* Windows encoding */
    HZ_CMAP_PLATFORM_CUSTOM = 4, /* Custom */
    /* Platform ID values 240 through 255 are reserved for user-defined platforms.
     * This specification will never assign these values to a registered platform.
     * Platform ID 2 (ISO) was deprecated as of OpenType version v1.3.
     * */
} hz_cmap_platform_t;


typedef enum hz_cmap_subtable_format_t {
    HZ_CMAP_SUBTABLE_FORMAT_BYTE_ENCODING_TABLE = 0,
    HZ_CMAP_SUBTABLE_FORMAT_SEGMENT_MAPPING_TO_DELTA_VALUES = 4
} hz_cmap_subtable_format_t;

//typedef enum hz_cmap_unicode_encoding_t {
//
//} hz_cmap_unicode_encoding_t;

typedef struct hz_cmap_encoding_t {
    uint16_t        platform_id; /* Platform ID. */
    uint16_t        encoding_id; /* Platform-specific encoding ID. */
    hz_offset32_t   subtable_offset; /* Byte offset from beginning of table to the subtable for this encoding. */
} hz_cmap_encoding_t;

static const char *
hz_cmap_platform_to_string(hz_cmap_platform_t platform) {
    switch (platform) {
        case HZ_CMAP_PLATFORM_UNICODE: return "Unicode";
        case HZ_CMAP_PLATFORM_MACINTOSH: return "Macintosh";
        case HZ_CMAP_PLATFORM_ISO: return "ISO";
        case HZ_CMAP_PLATFORM_WINDOWS: return "Windows";
        case HZ_CMAP_PLATFORM_CUSTOM: return "Custom";
        default: return NULL;
    }
}

/*
 * Format 0: Byte encoding table
 * https://docs.microsoft.com/en-us/typography/opentype/spec/cmap#format-0-byte-encoding-table
 * */
typedef struct hz_cmap_subtable_format0_t {
    uint16_t format; /* Format number is set to 0. */
    uint16_t length; /* This is the length in bytes of the subtable. */
    /* For requirements on use of the language field,
     * see “Use of the language field in 'cmap' subtables” in this document.
     * */
    uint16_t language;
    /* An array that maps character codes to glyph index values. */
    uint8_t glyph_id_array[256];
} hz_cmap_subtable_format0_t;


typedef struct hz_cmap_subtable_format4_t {
    uint16_t format;
    uint16_t length;
    uint16_t language;
    uint16_t seg_count_x2;
    uint16_t search_range;
    uint16_t entry_selector;
    uint16_t range_shift;
    uint16_t *end_code;
    uint16_t reserved_pad;
    uint16_t *start_code;
    int16_t *id_delta;
    uint16_t *id_range_offsets;
    uint16_t *glyph_id_array;
} hz_cmap_subtable_format4_t;

static hz_index_t
hz_cmap_unicode_to_id(hz_cmap_subtable_format4_t *st, hz_unicode_t c) {
    uint16_t range_count = st->seg_count_x2 >> 1;
    uint16_t i = 0;
    hz_index_t id;

    /* search for code range */
    while (i < range_count) {
        uint16_t start_code = bswap16(st->start_code[i]);
        uint16_t end_code = bswap16(st->end_code[i]);
        int16_t id_delta = bswap16(st->id_delta[i]);
        uint16_t id_range_offset = bswap16(st->id_range_offsets[i]);

        if (end_code >= c && start_code <= c) {
            if (id_range_offset != 0) {
                uint16_t raw_val = *(&st->id_range_offsets[i] + id_range_offset/2 + (c - start_code));
                id = bswap16( raw_val );
                if (id != 0) id += id_delta;
            } else
                id = id_delta + c;

            return id;
        }

        ++i;
    }

    return 0; /* map to .notdef */
}

hz_bool_t
hz_cmap_apply_encoding(hz_stream_t *table, hz_sequence_t *sequence,
                       hz_cmap_encoding_t enc)
{
    hz_stream_t *subtable = hz_stream_create(table->data + enc.subtable_offset, 0, 0);
    uint16_t format;
    hz_stream_read16(subtable, &format);

    switch (format) {
        case 0: break;
        case 2: break;
        case HZ_CMAP_SUBTABLE_FORMAT_SEGMENT_MAPPING_TO_DELTA_VALUES: {
            hz_cmap_subtable_format4_t st;
            hz_stream_read16(subtable, &st.length);
            hz_stream_read16(subtable, &st.language);
            hz_stream_read16(subtable, &st.seg_count_x2);
            hz_stream_read16(subtable, &st.search_range);
            hz_stream_read16(subtable, &st.entry_selector);
            hz_stream_read16(subtable, &st.range_shift);

            uint16_t seg_jmp = (st.seg_count_x2>>1) * sizeof(uint16_t);

            const uint8_t *curr_addr = subtable->data + subtable->offset;
            st.end_code = (uint16_t *)curr_addr;
            st.start_code = (uint16_t *)(curr_addr + seg_jmp + sizeof(uint16_t));
            st.id_delta = (int16_t *)(curr_addr + 2*seg_jmp + sizeof(uint16_t));
            st.id_range_offsets = (uint16_t *)(curr_addr + 3*seg_jmp + sizeof(uint16_t));

            /* map unicode characters to glyph indices in sequenceion */
            hz_sequence_node_t *curr_node = sequence->root;

            while (curr_node != NULL) {
                curr_node->id = hz_cmap_unicode_to_id(&st, curr_node->codepoint);
                curr_node = curr_node->next;
            }
            break;
        }
        default:
            return HZ_FALSE;
    }

    hz_stream_destroy(subtable);
    return HZ_TRUE;
}

hz_bool_t
hz_cmap_apply_encoding_to_set(hz_stream_t *table,
                              hz_set_t *codepoints,
                              hz_set_t *glyphs,
                              hz_cmap_encoding_t enc)
{
    hz_stream_t *subtable = hz_stream_create(table->data + enc.subtable_offset, 0, 0);
    uint16_t format;
    hz_stream_read16(subtable, &format);

    switch (format) {
        case 0: break;
        case 2: break;
        case HZ_CMAP_SUBTABLE_FORMAT_SEGMENT_MAPPING_TO_DELTA_VALUES: {
            hz_cmap_subtable_format4_t st;
            hz_stream_read16(subtable, &st.length);
            hz_stream_read16(subtable, &st.language);
            hz_stream_read16(subtable, &st.seg_count_x2);
            hz_stream_read16(subtable, &st.search_range);
            hz_stream_read16(subtable, &st.entry_selector);
            hz_stream_read16(subtable, &st.range_shift);

            uint16_t seg_jmp = (st.seg_count_x2>>1) * sizeof(uint16_t);

            const uint8_t *curr_addr = subtable->data + subtable->offset;
            st.end_code = (uint16_t *)curr_addr;
            st.start_code = (uint16_t *)(curr_addr + seg_jmp + sizeof(uint16_t));
            st.id_delta = (int16_t *)(curr_addr + 2*seg_jmp + sizeof(uint16_t));
            st.id_range_offsets = (uint16_t *)(curr_addr + 3*seg_jmp + sizeof(uint16_t));

            /* map unicode characters to glyph indices in sequenceion */
            size_t index = 0;
            size_t num_codes = codepoints->count;

            while (index < num_codes) {
                hz_set_add_no_duplicate(glyphs, hz_cmap_unicode_to_id(&st, codepoints->values[index]));
                ++index;
            }

            break;
        }
        default:
            return HZ_FALSE;
    }

    return HZ_TRUE;
}

void
hz_map_to_nominal_forms(hz_context_t *ctx,
                        hz_sequence_t *sequence)
{
    hz_face_t *face = hz_font_get_face(ctx->font);
    hz_blob_t *blob = hz_face_reference_table(face, HZ_TAG('c','m','a','p'));
    hz_stream_t *table = hz_blob_to_stream(blob);

    uint16_t version;
    hz_stream_read16(table, &version);

    /* Table version number must be 0 */
    HZ_ASSERT(version == 0);

    uint16_t num_encodings, enc_idx;
    hz_stream_read16(table, &num_encodings);

    HZ_LOG("cmap table encoding count: %d\n", num_encodings);

//    for (enc_idx = 0; enc_idx < num_encodings; ++enc_idx)
    {
        hz_cmap_encoding_t enc = {};
        hz_stream_read16(table, &enc.platform_id);
        hz_stream_read16(table, &enc.encoding_id);
        hz_stream_read32(table, &enc.subtable_offset);

        HZ_LOG("platform: %s\n", hz_cmap_platform_to_string(enc.platform_id));
        HZ_LOG("encoding: %d\n", enc.encoding_id);
        HZ_LOG("subtable-offset: %d\n", enc.subtable_offset);
        hz_cmap_apply_encoding(table, sequence, enc);
//        if (enc.platform_id == HZ_CMAP_PLATFORM_WINDOWS) {
//            hz_cmap_apply_encoding(table, sequence, enc);
//            break;
//        }


//        switch (enc.platform_id) {
//            case HZ_CMAP_PLATFORM_UNICODE: {
//                uint16_t subtable_format = 0;
//                hz_stream_t *subtable_stream = hz_stream_create(cmap_table->data + enc.subtable_offset,
//                                                                0, 0);
//                hz_stream_read16(subtable_stream, &subtable_format);
//
//                if (subtable_format == 4) {
//
//                }
//                break;
//            }
//            case HZ_CMAP_PLATFORM_MACINTOSH: {
//                break;
//            }
//            case HZ_CMAP_PLATFORM_ISO: {
//                break;
//            }
//            case HZ_CMAP_PLATFORM_WINDOWS: {
//                /* https://docs.microsoft.com/en-us/typography/opentype/spec/cmap#windows-platform-platform-id--3 */
//                switch (enc.encoding_id) {
//                    case 0: { /* Symbol */
//                        break;
//                    }
//                    case 1: { /* Unicode BMP */
//                        break;
//                    }
//                    case 2: { /* ShiftJIS */
//                        break;
//                    }
//                    case 3: { /* PRC */
//                        break;
//                    }
//                    case 4: { /* Big5 */
//                        break;
//                    }
//                    case 5: { /* Wansung */
//                        break;
//                    }
//                    case 6: { /* Johab */
//                        hz_cmap_windows_johab(cmap_table->data + enc.subtable_offset);
//                        break;
//                    }
//
////                    case 7 ... 9: { /* Reserved */
////                        break;
////                    }
//
//                    case 10: { /* Unicode full repertoire */
//                        hz_cmap_windows_unicode_full(cmap_table->data + enc.subtable_offset);
//                        break;
//                    }
//
//                    default:
//                        break;
//                }
//                break;
//            }
//
//            default: /* error */
//                break;
//        }
    }
    hz_stream_destroy(table);
}


void
hz_ot_parse_gdef_table(hz_context_t *ctx, hz_sequence_t *sequence)
{
    hz_face_t *face = hz_font_get_face(ctx->font);
    hz_stream_t *table = hz_stream_create(hz_face_get_ot_tables(face)->GDEF_table,0,0);
    uint32_t version;

    hz_offset16_t glyph_class_def_offset;
    hz_offset16_t attach_list_offset;
    hz_offset16_t lig_caret_list_offset;
    hz_offset16_t mark_attach_class_def_offset;

    hz_stream_read32(table, &version);

    switch (version) {
        case 0x00010000: /* 1.0 */
            hz_stream_read16(table, &glyph_class_def_offset);
            hz_stream_read16(table, &attach_list_offset);
            hz_stream_read16(table, &lig_caret_list_offset);
            hz_stream_read16(table, &mark_attach_class_def_offset);
            break;
        case 0x00010002: /* 1.2 */
            break;
        case 0x00010003: /* 1.3 */
            break;
        default: /* error */
            break;
    }

    if (glyph_class_def_offset != 0) {
        /* glyph class def isn't nil */
        hz_stream_t *subtable = hz_stream_create(table->data + glyph_class_def_offset, 0, 0);
        hz_map_t *class_map = hz_map_create();
        hz_sequence_node_t *curr_node = sequence->root;
        uint16_t class_format;
        hz_stream_read16(subtable, &class_format);
        switch (class_format) {
            case 1:
                break;
            case 2: {
                uint16_t range_index = 0, class_range_count;
                hz_stream_read16(subtable, &class_range_count);

                while (range_index < class_range_count) {
                    uint16_t start_glyph_id, end_glyph_id, glyph_class;
                    hz_stream_read16(subtable, &start_glyph_id);
                    hz_stream_read16(subtable, &end_glyph_id);
                    hz_stream_read16(subtable, &glyph_class);
                    HZ_ASSERT(glyph_class >= 1 && glyph_class <= 4);
                    hz_map_set_value_for_keys(class_map, start_glyph_id, end_glyph_id, HZ_BIT(glyph_class - 1));

                    ++range_index;
                }
                break;
            }
            default:
                break;
        }

        /* set glyph class values if in map */
        while (curr_node != NULL) {
            hz_index_t gid = curr_node->id;
            if (hz_map_value_exists(class_map, gid)) {
                curr_node->gc = hz_map_get_value(class_map, gid);
            } else {
                /* set default glyph class if current glyph id isn't found */
                curr_node->gc = HZ_GLYPH_CLASS_ZERO;
            }

            curr_node = curr_node->next;
        }

        hz_map_destroy(class_map);
        hz_stream_destroy(subtable);
    }

    hz_stream_destroy(table);
}


typedef struct hz_long_hor_metric_t {
    uint16_t advance_width; /* advance width */
    int16_t lsb; /* left side bearing */
} hz_long_hor_metric_t;

void
hz_read_h_metrics(hz_stream_t *table, size_t metrics_count, hz_long_hor_metric_t *metrics) {
    size_t index = 0;

    while (index < metrics_count) {
        hz_long_hor_metric_t *metric = &metrics[ index ];
        hz_stream_read16(table, &metric->advance_width);
        hz_stream_read16(table, (uint16_t *) &metric->lsb);
        ++index;
    }
}

/*
void
hz_read_lv_metrics();
*/

void
hz_fill_tt1_metrics(hz_face_t *face,
                    hz_metrics_t *metrics,
                    const hz_long_hor_metric_t *h_metrics,
                    const int16_t *left_side_bearings,
                    uint16_t num_of_h_glyphs,
                    uint16_t glyph_count)
{
    hz_blob_t *blob = hz_face_reference_table(face, HZ_TAG('g','l','y','f'));
    hz_stream_t *table = hz_blob_to_stream( blob );

    uint16_t ver;
    hz_stream_read16(table, &ver);

    switch (ver) {
        case 1: {
            uint16_t unk0, unk1, unk2;
            break;
        }
        case 2: {
            uint16_t unk0, unk1, unk2, unk3, unk4;
            hz_stream_read16(table, &unk0);
            hz_stream_read16(table, &unk1);
            hz_stream_read16(table, &unk2);
            hz_stream_read16(table, &unk3);
            hz_stream_read16(table, &unk4);
            break;
        }
        default:
            break;
    }
}

void
hz_apply_tt1_metrics(hz_face_t *face, hz_sequence_t *sequence)
{
    hz_long_hor_metric_t *h_metrics;
    int16_t *left_side_bearings;

    hz_blob_t *blob = hz_face_reference_table(face, HZ_TAG('h','m','t','x'));
    hz_stream_t *table = hz_blob_to_stream( blob );

    uint16_t num_of_h_metrics = hz_face_get_num_of_h_metrics(face);
    uint16_t glyph_count = hz_face_get_num_glyphs(face);
    uint16_t num_left_side_bearings = glyph_count - num_of_h_metrics;

    /* read long horizontal metrics */
    h_metrics = HZ_MALLOC(sizeof(hz_long_hor_metric_t) * num_of_h_metrics);
    hz_read_h_metrics(table, num_of_h_metrics, h_metrics);
    left_side_bearings = HZ_MALLOC(sizeof(int16_t) * num_left_side_bearings);
    hz_stream_read16_n(table, num_left_side_bearings, (uint16_t *)left_side_bearings);

    //hz_fill_tt1_metrics(face, metrics, h_metrics, left_side_bearings, num_of_h_metrics, glyph_count);

//    uint16_t glyph_index = 0;
//    while (glyph_index < glyph_count) {
//
//        hz_metrics_t *metric = &face->metrics[glyph_index];
//        if (glyph_index < num_of_h_metrics) {
//            hz_long_hor_metric_t *h_metric = &h_metrics[glyph_index];
//            metric->x_advance = h_metric->advance_width;
//            int16_t rsb = h_metrics->advance_width - (h_metrics->lsb + metric->x_max - metric->x_min);
//            metric->x_bearing = rsb + metric->width;
//        } else {
//            int16_t lsb = left_side_bearings[num_of_h_metrics + glyph_index];
//            int16_t rsb = lsb + (metric->x_max - metric->x_min);
//            metric->x_bearing = rsb;
//        }
//
//        ++glyph_index;
//    }

    /* apply the metrics to position the glyphs */
    hz_sequence_node_t *curr_node = sequence->root;
    while (curr_node != NULL) {
        hz_index_t id = curr_node->id;
        hz_metrics_t *metric = hz_face_get_glyph_metrics(face, id);

        curr_node->x_advance = metric->x_advance;
        curr_node->y_advance = metric->y_advance;
        curr_node->x_offset = 0;
        curr_node->y_offset = 0;

        curr_node = curr_node->next;
    }

    HZ_FREE(left_side_bearings);
    HZ_FREE(h_metrics);
}

static void
hz_apply_rtl_switch(hz_sequence_t *sequence) {
    hz_sequence_node_t *node = hz_sequence_last_node(sequence->root);
    sequence->root = node;

    while (node != NULL) {
        hz_sequence_node_t *prev, *next;
        prev = node->prev;
        next = node->next;
        node->next = prev;
        node->prev = next;
        node = prev;
    }
}

static void
hz_compute_sequence_width(hz_sequence_t *sequence) {
    hz_sequence_node_t *node = sequence->root;

    while (node != NULL) {
        sequence->width += node->x_advance;
        node = node->next;
    }
}

void
hz_setup_sequence_glyph_info(hz_context_t *ctx, hz_sequence_t *sequence) {
    hz_face_t *face = hz_font_get_face(ctx->font);
    const hz_face_ot_tables_t *tables = hz_face_get_ot_tables(face);
    if (tables->GDEF_table != NULL) {
        hz_ot_parse_gdef_table(ctx, sequence);
    } else {
        hz_sequence_node_t *node = sequence->root;

        while (node != NULL) {
            node->gc = HZ_GLYPH_CLASS_BASE;
            node = node->next;
        }
    }
}

void
hz_shape_full(hz_context_t *ctx, hz_sequence_t *sequence)
{
    hz_face_t *face = hz_font_get_face(ctx->font);
    hz_tag_t script_tag = hz_ot_script_to_tag(ctx->script);
    hz_tag_t language_tag = hz_ot_language_to_tag(ctx->language);
    const hz_face_ot_tables_t *tables = hz_face_get_ot_tables(face);

    /* map unicode characters to nominal glyph indices */
    hz_map_to_nominal_forms(ctx, sequence);

    /* sets glyph class information */
    hz_setup_sequence_glyph_info(ctx, sequence);

    /* substitute glyphs */
    if (tables->GSUB_table != NULL)
        hz_ot_layout_apply_gsub_features(face, script_tag,
                                         language_tag,
                                         ctx->features,
                                         sequence);

    /* position glyphs */
    hz_apply_tt1_metrics(face, sequence);
    if (tables->GPOS_table != NULL)
        hz_ot_layout_apply_gpos_features(face, script_tag,
                                         language_tag,
                                         ctx->features,
                                         sequence);

    if (ctx->dir == HZ_DIRECTION_RTL)
        hz_apply_rtl_switch(sequence);

    hz_compute_sequence_width(sequence);
}

void
hz_decode_hhea_table(hz_face_t *face, hz_blob_t *blob)
{
    hz_stream_t *table = hz_blob_to_stream(blob);

    uint32_t version;
    FWORD ascender, descender, line_gap;
    UFWORD advance_width_max;
    FWORD min_left_side_bearing;
    FWORD min_right_side_bearing;
    FWORD x_max_extent;
    int16_t caret_slope_rise;
    int16_t caret_slope_run;
    int16_t caret_offset;
    int16_t metric_data_format;
    uint16_t num_of_h_metrics;

    hz_stream_read32(table, &version);

    if (version == 0x00010000) {
        /* version 1.0 */
        hz_stream_read16(table, (uint16_t *) &ascender);
        hz_stream_read16(table, (uint16_t *) &descender);
        hz_stream_read16(table, (uint16_t *) &line_gap);
        hz_stream_read16(table, &advance_width_max);
        hz_stream_read16(table, (uint16_t *) &min_left_side_bearing);
        hz_stream_read16(table, (uint16_t *) &min_right_side_bearing);
        hz_stream_read16(table, (uint16_t *) &x_max_extent);
        hz_stream_read16(table, (uint16_t *) &caret_slope_rise);
        hz_stream_read16(table, (uint16_t *) &caret_slope_run);
        hz_stream_read16(table, (uint16_t *) &caret_offset);

        /* skip over 8 bytes of reserved space */
        hz_stream_seek(table, 8);

        hz_stream_read16(table, (uint16_t *) &metric_data_format);
        hz_stream_read16(table, &num_of_h_metrics);
    } else {
        /* error */
    }

    hz_face_set_num_of_h_metrics(face, num_of_h_metrics);
}


static hz_set_t *
hz_gather_script_codepoints(hz_script_t script)
{
    hz_set_t *codepoints = hz_set_create();
    int i;

    for (i = 0; i < HZ_ARRAY_SIZE(script_ranges); ++i) {
        hz_script_range_t range = script_ranges[i];
        if (range.script == script) {
            hz_set_add_range_no_duplicate(codepoints, range.first_code, range.last_code);
        }
    }

    return codepoints;
}

static void
hz_gather_script_glyphs(hz_face_t *face, hz_script_t script, hz_set_t *glyphs)
{
    hz_set_t *codepoints = hz_gather_script_codepoints(script);
    hz_blob_t *cmap_blob = hz_face_reference_table(face, HZ_TAG('c','m','a','p'));
    hz_stream_t *table = hz_blob_to_stream(cmap_blob);

    uint16_t version;
    hz_stream_read16(table, &version);

    if (version == 0) {
        uint16_t num_encodings, enc_idx;
        hz_stream_read16(table, &num_encodings);

        hz_cmap_encoding_t enc = {};
        hz_stream_read16(table, &enc.platform_id);
        hz_stream_read16(table, &enc.encoding_id);
        hz_stream_read32(table, &enc.subtable_offset);

        hz_cmap_apply_encoding_to_set(table, codepoints, glyphs, enc);

    } else {
        /* error, table version must be 0 */
        HZ_ERROR("cmap table version must be zero!");
    }

    hz_set_destroy(codepoints);
}

hz_set_t *
hz_context_gather_required_glyphs(hz_context_t *ctx)
{
    hz_set_t *glyphs = hz_set_create();
    hz_face_t *face = hz_font_get_face(ctx->font);
    hz_tag_t script_tag = hz_ot_script_to_tag(ctx->script);
    hz_tag_t language_tag = hz_ot_language_to_tag(ctx->language);

    hz_gather_script_glyphs(face, HZ_SCRIPT_COMMON, glyphs);
    hz_gather_script_glyphs(face, HZ_SCRIPT_INHERITED, glyphs);
    hz_gather_script_glyphs(face, ctx->script, glyphs);

    if (hz_face_get_ot_tables(face)->GSUB_table != NULL)
        hz_ot_layout_gather_glyphs(face, script_tag, language_tag, ctx->features, glyphs);

    return glyphs;
}