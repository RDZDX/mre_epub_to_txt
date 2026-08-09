#include "vmsys.h"
#include "vmio.h"
#include "vmgraph.h"
#include "vmchset.h"
#include "vmstdlib.h"
#include "vm4res.h"
#include "vmres.h"

#include <stdio.h>
#include <string.h>

enum AppState {
    STATE_READY,
    STATE_CONVERTING,
    STATE_DONE,
};

static VMCHAR kInputPath[] = "E:\\epub\\book.epub";
static VMCHAR kOutputPath[] = "E:\\epub\\book.txt";
static const size_t kMaxOutputBytes = 512 * 1024;
static const size_t kMaxEpubBytes = 512 * 1024;
static const size_t kMaxInflateBytes = 256 * 1024;
static const VMWCHAR kTitleText[] = { 'E', 'P', 'U', 'B', 0x2192, 'T', 'X', 'T', 0 };

VMINT layer_hdl[1];
VMUINT8 *layer_buf = 0;
VMINT screen_w = 0;
VMINT screen_h = 0;

static AppState g_state = STATE_READY;
static char g_status_line1[80] = "Ready";
static char g_status_line2[160] = "";

static void handle_sysevt(VMINT message, VMINT param);
static void handle_keyevt(VMINT event, VMINT keycode);
static void handle_penevt(VMINT event, VMINT x, VMINT y);

struct HuffmanTable {
    unsigned short count[16];
    unsigned short symbol[320];
};

struct BitReader {
    const unsigned char *src;
    size_t size;
    size_t pos;
    unsigned int bitbuf;
    int bitcnt;
};

struct TextWriter {
    VMFILE file;
    char buffer[1024];
    size_t used;
    size_t total_written;
    int truncated;
    int io_error;
    int pending_space;
    int pending_newline;
};

struct HtmlStripState {
    char tag_buf[96];
    int tag_len;
    int in_tag;
    int skip_mode;
};

enum SkipMode {
    SKIP_NONE = 0,
    SKIP_STYLE = 1,
    SKIP_SCRIPT = 2,
    SKIP_HEAD = 3,
};

static void copy_ascii(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0) {
        return;
    }
    size_t i = 0;
    if (src) {
        for (; i + 1 < dst_size && src[i]; ++i) {
            dst[i] = src[i];
        }
    }
    dst[i] = '\0';
}

static void set_status(const char *line1, const char *line2) {
    copy_ascii(g_status_line1, sizeof(g_status_line1), line1);
    copy_ascii(g_status_line2, sizeof(g_status_line2), line2);
}

static unsigned char ascii_lower(unsigned char c) {
    if (c >= 'A' && c <= 'Z') {
        return (unsigned char)(c - 'A' + 'a');
    }
    return c;
}

static int ascii_is_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

static int ascii_equals(const char *text, const char *value) {
    size_t i = 0;
    while (text[i] && value[i]) {
        if (ascii_lower((unsigned char)text[i]) != ascii_lower((unsigned char)value[i])) {
            return 0;
        }
        ++i;
    }
    return text[i] == '\0' && value[i] == '\0';
}

static int is_block_tag(const char *name) {
    if (ascii_equals(name, "br") || ascii_equals(name, "p") || ascii_equals(name, "div") ||
        ascii_equals(name, "li") || ascii_equals(name, "tr") || ascii_equals(name, "td") ||
        ascii_equals(name, "th") || ascii_equals(name, "table") || ascii_equals(name, "section") ||
        ascii_equals(name, "article") || ascii_equals(name, "blockquote") ||
        ascii_equals(name, "body")) {
        return 1;
    }
    return name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && name[2] == '\0';
}

static unsigned short read_le16(const unsigned char *p) {
    return (unsigned short)(p[0] | (p[1] << 8));
}

static unsigned int read_le32(const unsigned char *p) {
    return (unsigned int)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

static void set_color(VMUINT16 color_value) {
    vm_graphic_color color;
    color.vm_color_565 = color_value;
    vm_graphic_setcolor(&color);
}

static void draw_ascii_line(int x, int y, const char *text, VMUINT16 color_value)
{
    VMWCHAR ws[256];
    set_color(color_value);
    vm_ascii_to_ucs2(ws, sizeof(ws), (VMSTR)text);
    vm_graphic_textout_to_layer(layer_hdl[0], x, y, ws, vm_wstrlen(ws));
}

static void draw_centered_wide(int y, const VMWCHAR *text, VMUINT16 color_value) {
    int width = vm_graphic_get_string_width((VMWSTR)text);
    int x = (screen_w - width) / 2;
    if (x < 0) {
        x = 0;
    }
    set_color(color_value);
    vm_graphic_textout_to_layer(layer_hdl[0], x, y, (VMWSTR)text, vm_wstrlen((VMWSTR)text));
}

static void draw_centered_ascii(int y, const char *text, VMUINT16 color_value)
{
    VMWCHAR ws[256];

    vm_ascii_to_ucs2(ws, sizeof(ws), (VMSTR)text);

    draw_centered_wide(y, ws, color_value);
}

static void draw_screen(void) {
    if (layer_hdl[0] == -1) {
        return;
    }

    set_color(VM_COLOR_WHITE);
    vm_graphic_fill_rect_ex(layer_hdl[0], 0, 0, screen_w, screen_h);

    if (g_state == STATE_READY) {
        draw_centered_wide(18, kTitleText, VM_COLOR_BLUE);
        draw_ascii_line(12, screen_h / 2 - 40, "Input:", VM_COLOR_BLACK);
        draw_ascii_line(12, screen_h / 2 - 16, kInputPath, VM_COLOR_BLUE);
        draw_ascii_line(12, screen_h - 42, "OK=Convert", VM_COLOR_BLACK);
        draw_ascii_line(12, screen_h - 22, "Back=Exit", VM_COLOR_BLACK);
    } else if (g_state == STATE_CONVERTING) {
        draw_centered_ascii(screen_h / 2 - vm_graphic_get_character_height() / 2,
                            "Converting...", VM_COLOR_BLUE);
    } else {
        draw_centered_ascii(56, g_status_line1, VM_COLOR_BLUE);
        draw_ascii_line(12, 100, g_status_line2, VM_COLOR_BLACK);
        draw_ascii_line(12, screen_h - 22, "Any key=Back", VM_COLOR_BLACK);
    }

    vm_graphic_flush_layer(layer_hdl, 1);
}

static void ensure_layer(void) {
    if (layer_hdl[0] == -1) {
        layer_hdl[0] = vm_graphic_create_layer(0, 0, screen_w, screen_h, -1);
        layer_buf = vm_graphic_get_layer_buffer(layer_hdl[0]);
        vm_graphic_set_clip(0, 0, screen_w, screen_h);
    }
}

static void release_layer(void) {
    if (layer_hdl[0] != -1) {
        vm_graphic_delete_layer(layer_hdl[0]);
        layer_hdl[0] = -1;
        layer_buf = 0;
    }
}

static int writer_flush(TextWriter *writer) {
    if (!writer || writer->io_error || writer->used == 0) {
        return writer && !writer->io_error;
    }
    VMUINT written = 0;
    if (vm_file_write(writer->file, writer->buffer, (VMUINT)writer->used, &written) < 0 ||
        written != writer->used) {
        writer->io_error = 1;
        return 0;
    }
    writer->total_written += writer->used;
    writer->used = 0;
    return 1;
}

static int writer_put_raw_byte(TextWriter *writer, unsigned char byte) {
    if (!writer || writer->io_error) {
        return 0;
    }
    if (writer->truncated) {
        return 1;
    }
    if (writer->total_written + writer->used >= kMaxOutputBytes) {
        writer->truncated = 1;
        return 1;
    }
    if (writer->used >= sizeof(writer->buffer) && !writer_flush(writer)) {
        return 0;
    }
    writer->buffer[writer->used++] = (char)byte;
    return 1;
}

static int writer_emit_pending(TextWriter *writer) {
    if (!writer || writer->io_error) {
        return 0;
    }
    if (writer->pending_newline) {
        writer->pending_newline = 0;
        writer->pending_space = 0;
        if (writer->total_written > 0 || writer->used > 0) {
            return writer_put_raw_byte(writer, '\n');
        }
        return 1;
    }
    if (writer->pending_space) {
        writer->pending_space = 0;
        if (writer->total_written > 0 || writer->used > 0) {
            return writer_put_raw_byte(writer, ' ');
        }
    }
    return 1;
}

static int writer_append_ascii_char(TextWriter *writer, unsigned char c) {
    if (ascii_is_space(c)) {
        if (c == '\r' || c == '\n') {
            writer->pending_newline = 1;
            writer->pending_space = 0;
        } else if (!writer->pending_newline) {
            writer->pending_space = 1;
        }
        return 1;
    }
    if (!writer_emit_pending(writer)) {
        return 0;
    }
    return writer_put_raw_byte(writer, c);
}

static int writer_append_utf8(TextWriter *writer, unsigned int codepoint) {
    unsigned char utf8[4];
    int count = 0;
    if (codepoint <= 0x7F) {
        return writer_append_ascii_char(writer, (unsigned char)codepoint);
    }
    if (codepoint <= 0x7FF) {
        utf8[0] = (unsigned char)(0xC0 | (codepoint >> 6));
        utf8[1] = (unsigned char)(0x80 | (codepoint & 0x3F));
        count = 2;
    } else if (codepoint <= 0xFFFF) {
        utf8[0] = (unsigned char)(0xE0 | (codepoint >> 12));
        utf8[1] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3F));
        utf8[2] = (unsigned char)(0x80 | (codepoint & 0x3F));
        count = 3;
    } else if (codepoint <= 0x10FFFF) {
        utf8[0] = (unsigned char)(0xF0 | (codepoint >> 18));
        utf8[1] = (unsigned char)(0x80 | ((codepoint >> 12) & 0x3F));
        utf8[2] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3F));
        utf8[3] = (unsigned char)(0x80 | (codepoint & 0x3F));
        count = 4;
    } else {
        return 0;
    }
    if (!writer_emit_pending(writer)) {
        return 0;
    }
    for (int i = 0; i < count; ++i) {
        if (!writer_put_raw_byte(writer, utf8[i])) {
            return 0;
        }
    }
    return 1;
}

static int writer_append_data(TextWriter *writer, const unsigned char *data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        unsigned char c = data[i];
        if (c < 0x80) {
            if (!writer_append_ascii_char(writer, c)) {
                return 0;
            }
        } else {
            if (!writer_emit_pending(writer) || !writer_put_raw_byte(writer, c)) {
                return 0;
            }
        }
    }
    return 1;
}

static void writer_newline(TextWriter *writer) {
    if (writer) {
        writer->pending_newline = 1;
        writer->pending_space = 0;
    }
}

static int bitreader_pull(BitReader *br, int need, unsigned int *value) {
    while (br->bitcnt < need) {
        if (br->pos >= br->size) {
            return 0;
        }
        br->bitbuf |= (unsigned int)br->src[br->pos++] << br->bitcnt;
        br->bitcnt += 8;
    }
    if (need == 0) {
        *value = 0;
        return 1;
    }
    *value = br->bitbuf & ((1u << need) - 1u);
    br->bitbuf >>= need;
    br->bitcnt -= need;
    return 1;
}

static int huffman_build(HuffmanTable *table, const unsigned char *lengths, int count) {
    unsigned short offsets[16];
    int left = 1;
    memset(table, 0, sizeof(*table));
    for (int symbol = 0; symbol < count; ++symbol) {
        if (lengths[symbol] > 15) {
            return 0;
        }
        table->count[lengths[symbol]]++;
    }
    table->count[0] = 0;
    for (int len = 1; len <= 15; ++len) {
        left <<= 1;
        left -= table->count[len];
        if (left < 0) {
            return 0;
        }
    }
    offsets[1] = 0;
    for (int len = 1; len < 15; ++len) {
        offsets[len + 1] = (unsigned short)(offsets[len] + table->count[len]);
    }
    for (int symbol = 0; symbol < count; ++symbol) {
        unsigned char len = lengths[symbol];
        if (len != 0) {
            table->symbol[offsets[len]++] = (unsigned short)symbol;
        }
    }
    return 1;
}

static int huffman_decode(BitReader *br, const HuffmanTable *table, unsigned int *symbol) {
    unsigned int code = 0;
    unsigned int first = 0;
    unsigned int index = 0;
    for (int len = 1; len <= 15; ++len) {
        unsigned int bit = 0;
        if (!bitreader_pull(br, 1, &bit)) {
            return 0;
        }
        code |= bit;
        if (code < first + table->count[len]) {
            *symbol = table->symbol[index + (code - first)];
            return 1;
        }
        index += table->count[len];
        first = (first + table->count[len]) << 1;
        code <<= 1;
    }
    return 0;
}

static int inflate_codes(BitReader *br, const HuffmanTable *litlen, const HuffmanTable *dist,
                         unsigned char *out, size_t out_size, size_t *out_pos) {
    static const unsigned short kLengthBase[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
        31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
    };
    static const unsigned char kLengthExtra[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
        2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
    };
    static const unsigned short kDistBase[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25,
        33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
        1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
    };
    static const unsigned char kDistExtra[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3,
        4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
        9, 9, 10, 10, 11, 11, 12, 12, 13, 13
    };

    while (1) {
        unsigned int sym = 0;
        if (!huffman_decode(br, litlen, &sym)) {
            return 0;
        }
        if (sym < 256) {
            if (*out_pos >= out_size) {
                return 0;
            }
            out[(*out_pos)++] = (unsigned char)sym;
            continue;
        }
        if (sym == 256) {
            return 1;
        }
        if (sym < 257 || sym > 285) {
            return 0;
        }

        unsigned int extra = 0;
        unsigned int length = kLengthBase[sym - 257];
        if (kLengthExtra[sym - 257] && !bitreader_pull(br, kLengthExtra[sym - 257], &extra)) {
            return 0;
        }
        length += extra;

        unsigned int dist_sym = 0;
        if (!huffman_decode(br, dist, &dist_sym) || dist_sym > 29) {
            return 0;
        }
        extra = 0;
        unsigned int distance = kDistBase[dist_sym];
        if (kDistExtra[dist_sym] && !bitreader_pull(br, kDistExtra[dist_sym], &extra)) {
            return 0;
        }
        distance += extra;
        if (distance == 0 || distance > *out_pos) {
            return 0;
        }
        while (length--) {
            if (*out_pos >= out_size) {
                return 0;
            }
            out[*out_pos] = out[*out_pos - distance];
            ++(*out_pos);
        }
    }
}

static int inflate_dynamic_tables(BitReader *br, HuffmanTable *litlen, HuffmanTable *dist) {
    static const unsigned char kCodeOrder[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    unsigned int hlit = 0, hdist = 0, hclen = 0;
    unsigned char code_lengths[19];
    unsigned char lengths[320];
    HuffmanTable code_table;

    memset(code_lengths, 0, sizeof(code_lengths));
    memset(lengths, 0, sizeof(lengths));

    if (!bitreader_pull(br, 5, &hlit) || !bitreader_pull(br, 5, &hdist) ||
        !bitreader_pull(br, 4, &hclen)) {
        return 0;
    }
    hlit += 257;
    hdist += 1;
    hclen += 4;
    if (hlit > 286 || hdist > 32) {
        return 0;
    }

    for (unsigned int i = 0; i < hclen; ++i) {
        unsigned int value = 0;
        if (!bitreader_pull(br, 3, &value)) {
            return 0;
        }
        code_lengths[kCodeOrder[i]] = (unsigned char)value;
    }
    if (!huffman_build(&code_table, code_lengths, 19)) {
        return 0;
    }

    for (unsigned int i = 0; i < hlit + hdist;) {
        unsigned int sym = 0;
        if (!huffman_decode(br, &code_table, &sym)) {
            return 0;
        }
        if (sym <= 15) {
            lengths[i++] = (unsigned char)sym;
        } else if (sym == 16) {
            unsigned int repeat = 0;
            if (i == 0 || !bitreader_pull(br, 2, &repeat)) {
                return 0;
            }
            repeat += 3;
            while (repeat-- && i < hlit + hdist) {
                lengths[i] = lengths[i - 1];
                ++i;
            }
        } else if (sym == 17) {
            unsigned int repeat = 0;
            if (!bitreader_pull(br, 3, &repeat)) {
                return 0;
            }
            repeat += 3;
            while (repeat-- && i < hlit + hdist) {
                lengths[i++] = 0;
            }
        } else if (sym == 18) {
            unsigned int repeat = 0;
            if (!bitreader_pull(br, 7, &repeat)) {
                return 0;
            }
            repeat += 11;
            while (repeat-- && i < hlit + hdist) {
                lengths[i++] = 0;
            }
        } else {
            return 0;
        }
    }

    if (!huffman_build(litlen, lengths, (int)hlit) ||
        !huffman_build(dist, lengths + hlit, (int)hdist)) {
        return 0;
    }
    return 1;
}

static int inflate_raw_deflate(const unsigned char *src, size_t src_size,
                               unsigned char *dst, size_t dst_size, size_t *written) {
    unsigned char fixed_ll_lengths[288];
    unsigned char fixed_dist_lengths[32];
    HuffmanTable fixed_ll;
    HuffmanTable fixed_dist;
    BitReader br;
    size_t out_pos = 0;

    for (int i = 0; i <= 143; ++i) fixed_ll_lengths[i] = 8;
    for (int i = 144; i <= 255; ++i) fixed_ll_lengths[i] = 9;
    for (int i = 256; i <= 279; ++i) fixed_ll_lengths[i] = 7;
    for (int i = 280; i <= 287; ++i) fixed_ll_lengths[i] = 8;
    for (int i = 0; i < 32; ++i) fixed_dist_lengths[i] = 5;
    if (!huffman_build(&fixed_ll, fixed_ll_lengths, 288) ||
        !huffman_build(&fixed_dist, fixed_dist_lengths, 32)) {
        return 0;
    }

    br.src = src;
    br.size = src_size;
    br.pos = 0;
    br.bitbuf = 0;
    br.bitcnt = 0;

    while (1) {
        unsigned int final_block = 0;
        unsigned int block_type = 0;
        if (!bitreader_pull(&br, 1, &final_block) || !bitreader_pull(&br, 2, &block_type)) {
            return 0;
        }

        if (block_type == 0) {
            unsigned int len = 0;
            unsigned int nlen = 0;
            br.bitbuf = 0;
            br.bitcnt = 0;
            if (br.pos + 4 > br.size) {
                return 0;
            }
            len = read_le16(br.src + br.pos);
            nlen = read_le16(br.src + br.pos + 2);
            br.pos += 4;
            if ((len ^ 0xFFFFu) != nlen || br.pos + len > br.size || out_pos + len > dst_size) {
                return 0;
            }
            memcpy(dst + out_pos, br.src + br.pos, len);
            out_pos += len;
            br.pos += len;
        } else if (block_type == 1 || block_type == 2) {
            HuffmanTable litlen;
            HuffmanTable dist;
            if (block_type == 1) {
                litlen = fixed_ll;
                dist = fixed_dist;
            } else if (!inflate_dynamic_tables(&br, &litlen, &dist)) {
                return 0;
            }
            if (!inflate_codes(&br, &litlen, &dist, dst, dst_size, &out_pos)) {
                return 0;
            }
        } else {
            return 0;
        }

        if (final_block) {
            break;
        }
    }

    *written = out_pos;
    return 1;
}

static int entity_match(const char *entity, size_t len, const char *name) {
    size_t i = 0;
    while (i < len && name[i]) {
        if (ascii_lower((unsigned char)entity[i]) != ascii_lower((unsigned char)name[i])) {
            return 0;
        }
        ++i;
    }
    return i == len && name[i] == '\0';
}

static int decode_entity(TextWriter *writer, const unsigned char *entity, size_t len) {
    if (len == 0) {
        return 0;
    }
    if (entity[0] == '#') {
        unsigned int value = 0;
        size_t i = 1;
        int hex = 0;
        if (i < len && (entity[i] == 'x' || entity[i] == 'X')) {
            hex = 1;
            ++i;
        }
        for (; i < len; ++i) {
            unsigned char c = entity[i];
            if (hex) {
                if (c >= '0' && c <= '9') value = (value << 4) + (c - '0');
                else if (c >= 'a' && c <= 'f') value = (value << 4) + (c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') value = (value << 4) + (c - 'A' + 10);
                else return 0;
            } else {
                if (c < '0' || c > '9') return 0;
                value = value * 10 + (c - '0');
            }
        }
        if (value == 10 || value == 13) {
            writer_newline(writer);
            return 1;
        }
        if (value == 9 || value == 32) {
            writer->pending_space = 1;
            return 1;
        }
        return writer_append_utf8(writer, value);
    }

    if (entity_match((const char *)entity, len, "amp")) return writer_append_ascii_char(writer, '&');
    if (entity_match((const char *)entity, len, "lt")) return writer_append_ascii_char(writer, '<');
    if (entity_match((const char *)entity, len, "gt")) return writer_append_ascii_char(writer, '>');
    if (entity_match((const char *)entity, len, "nbsp")) {
        writer->pending_space = 1;
        return 1;
    }
    if (entity_match((const char *)entity, len, "quot")) return writer_append_ascii_char(writer, '"');
    if (entity_match((const char *)entity, len, "apos")) return writer_append_ascii_char(writer, '\'');
    return 0;
}

static void finish_tag(HtmlStripState *state, TextWriter *writer) {
    char name[20];
    int i = 0;
    int j = 0;
    int is_end = 0;

    while (i < state->tag_len && ascii_is_space((unsigned char)state->tag_buf[i])) {
        ++i;
    }
    if (i < state->tag_len && state->tag_buf[i] == '/') {
        is_end = 1;
        ++i;
        while (i < state->tag_len && ascii_is_space((unsigned char)state->tag_buf[i])) {
            ++i;
        }
    }
    while (i < state->tag_len && j + 1 < (int)sizeof(name)) {
        unsigned char c = ascii_lower((unsigned char)state->tag_buf[i]);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '!' || c == '?') {
            name[j++] = (char)c;
            ++i;
        } else {
            break;
        }
    }
    name[j] = '\0';

    if (state->skip_mode != SKIP_NONE) {
        if (is_end &&
            ((state->skip_mode == SKIP_STYLE && ascii_equals(name, "style")) ||
             (state->skip_mode == SKIP_SCRIPT && ascii_equals(name, "script")) ||
             (state->skip_mode == SKIP_HEAD && ascii_equals(name, "head")))) {
            state->skip_mode = SKIP_NONE;
        }
        return;
    }

    if (!is_end) {
        if (ascii_equals(name, "style")) {
            state->skip_mode = SKIP_STYLE;
            return;
        }
        if (ascii_equals(name, "script")) {
            state->skip_mode = SKIP_SCRIPT;
            return;
        }
        if (ascii_equals(name, "head")) {
            state->skip_mode = SKIP_HEAD;
            return;
        }
    }

    if (is_block_tag(name)) {
        writer_newline(writer);
    }
}

static int strip_html_to_writer(const unsigned char *html, size_t html_size, TextWriter *writer) {
    HtmlStripState state;
    memset(&state, 0, sizeof(state));

    for (size_t i = 0; i < html_size; ++i) {
        unsigned char c = html[i];
        if (state.in_tag) {
            if (c == '>') {
                finish_tag(&state, writer);
                state.in_tag = 0;
                state.tag_len = 0;
            } else if (state.tag_len + 1 < (int)sizeof(state.tag_buf)) {
                state.tag_buf[state.tag_len++] = (char)c;
            }
            continue;
        }

        if (state.skip_mode != SKIP_NONE) {
            if (c == '<') {
                state.in_tag = 1;
                state.tag_len = 0;
            }
            continue;
        }

        if (c == '<') {
            state.in_tag = 1;
            state.tag_len = 0;
            continue;
        }

        if (c == '&') {
            size_t start = i + 1;
            size_t end = start;
            while (end < html_size && end - start < 16 && html[end] != ';' &&
                   html[end] != '<' && html[end] != '&') {
                ++end;
            }
            if (end < html_size && html[end] == ';' && decode_entity(writer, html + start, end - start)) {
                i = end;
                continue;
            }
        }

        if (!writer_append_data(writer, &c, 1)) {
            return 0;
        }
    }

    writer_newline(writer);
    return writer_emit_pending(writer) && writer_flush(writer);
}

static int find_eocd(const unsigned char *zip, size_t zip_size, size_t *offset) {
    const size_t minimum = zip_size > 0xFFFFu + 22u ? zip_size - (0xFFFFu + 22u) : 0;
    if (zip_size < 22) {
        return 0;
    }
    for (size_t pos = zip_size - 22;; --pos) {
        if (zip[pos] == 0x50 && zip[pos + 1] == 0x4B && zip[pos + 2] == 0x05 && zip[pos + 3] == 0x06) {
            *offset = pos;
            return 1;
        }
        if (pos == minimum) {
            break;
        }
    }
    return 0;
}

static int has_html_extension(const unsigned char *name, size_t len) {
    static const char *exts[] = { ".xhtml", ".html", ".htm" };
    for (size_t e = 0; e < sizeof(exts) / sizeof(exts[0]); ++e) {
        size_t ext_len = strlen(exts[e]);
        if (len < ext_len) {
            continue;
        }
        size_t start = len - ext_len;
        int match = 1;
        for (size_t i = 0; i < ext_len; ++i) {
            if (ascii_lower(name[start + i]) != (unsigned char)exts[e][i]) {
                match = 0;
                break;
            }
        }
        if (match) {
            return 1;
        }
    }
    return 0;
}

static int process_zip_entry(const unsigned char *zip, size_t zip_size,
                             unsigned int local_offset, unsigned short method,
                             unsigned int compressed_size, unsigned int uncompressed_size,
                             TextWriter *writer, char *error_text, size_t error_size) {
    if (local_offset + 30u > zip_size || read_le32(zip + local_offset) != 0x04034B50u) {
        copy_ascii(error_text, error_size, "Bad local header");
        return 0;
    }
    unsigned short name_len = read_le16(zip + local_offset + 26);
    unsigned short extra_len = read_le16(zip + local_offset + 28);
    size_t data_offset = local_offset + 30u + name_len + extra_len;
    if (data_offset + compressed_size > zip_size) {
        copy_ascii(error_text, error_size, "ZIP entry overflow");
        return 0;
    }

    const unsigned char *src = 0;
    unsigned char *inflated = 0;
    size_t inflated_size = 0;

    if (method == 0) {
        if (compressed_size != uncompressed_size) {
            copy_ascii(error_text, error_size, "Bad stored entry");
            return 0;
        }
        src = zip + data_offset;
        inflated_size = uncompressed_size;
    } else if (method == 8) {
        if (uncompressed_size == 0) {
            inflated_size = 0;
        } else {
            if (uncompressed_size > kMaxInflateBytes) {
                copy_ascii(error_text, error_size, "HTML entry too large");
                return 0;
            }
            inflated = (unsigned char *)vm_malloc(uncompressed_size);
            if (!inflated) {
                copy_ascii(error_text, error_size, "No RAM for inflate");
                return 0;
            }
            if (!inflate_raw_deflate(zip + data_offset, compressed_size, inflated,
                                     uncompressed_size, &inflated_size) ||
                inflated_size != uncompressed_size) {
                vm_free(inflated);
                copy_ascii(error_text, error_size, "Deflate failed");
                return 0;
            }
            src = inflated;
        }
    } else {
        copy_ascii(error_text, error_size, "ZIP method unsupported");
        return 0;
    }

    if (inflated_size == 0) {
        if (inflated) {
            vm_free(inflated);
        }
        return 1;
    }

    if (writer->total_written > 0 || writer->used > 0) {
        writer_newline(writer);
    }
    int ok = strip_html_to_writer(src, inflated_size, writer);
    if (inflated) {
        vm_free(inflated);
    }
    if (!ok) {
        copy_ascii(error_text, error_size, "Write failed");
        return 0;
    }
    return 1;
}

static int convert_epub_to_txt(char *error_text, size_t error_size, int *truncated) {
    VMWCHAR input_path[256];
    VMWCHAR output_path[256];
    VMFILE in_file = -1;
    VMFILE out_file = -1;
    unsigned char *zip = 0;
    TextWriter writer;
    size_t eocd = 0;
    int found = 0;

    *truncated = 0;
    memset(&writer, 0, sizeof(writer));

    vm_ascii_to_ucs2(input_path, sizeof(input_path), kInputPath);
    vm_ascii_to_ucs2(output_path, sizeof(output_path), kOutputPath);

    in_file = vm_file_open(input_path, MODE_READ, VM_TRUE);
    if (in_file < 0) {
        copy_ascii(error_text, error_size, "Cannot open EPUB");
        return 0;
    }

    //VMINT size_hi = 0;
    VMUINT size_hi = 0;
    VMINT size_lo = vm_file_getfilesize(in_file, &size_hi);
    if (size_hi != 0 || size_lo <= 0 || (size_t)size_lo > kMaxEpubBytes) {
        copy_ascii(error_text, error_size, "EPUB too large for RAM");
        vm_file_close(in_file);
        return 0;
    }

    zip = (unsigned char *)vm_malloc((size_t)size_lo);
    if (!zip) {
        copy_ascii(error_text, error_size, "No RAM for EPUB");
        vm_file_close(in_file);
        return 0;
    }

    VMUINT read_count = 0;
    if (vm_file_read(in_file, zip, (VMUINT)size_lo, &read_count) < 0 || read_count != (VMUINT)size_lo) {
        copy_ascii(error_text, error_size, "Read failed");
        vm_free(zip);
        vm_file_close(in_file);
        return 0;
    }
    vm_file_close(in_file);

    if (!find_eocd(zip, (size_t)size_lo, &eocd)) {
        copy_ascii(error_text, error_size, "ZIP footer missing");
        vm_free(zip);
        return 0;
    }

    out_file = vm_file_open(output_path, MODE_CREATE_ALWAYS_WRITE, VM_TRUE);
    if (out_file < 0) {
        copy_ascii(error_text, error_size, "Cannot create TXT");
        vm_free(zip);
        return 0;
    }

    writer.file = out_file;

    unsigned int entry_count = read_le16(zip + eocd + 10);
    unsigned int cd_offset = read_le32(zip + eocd + 16);
    size_t pos = cd_offset;
    if (cd_offset >= (unsigned int)size_lo) {
        copy_ascii(error_text, error_size, "Bad central dir");
        vm_free(zip);
        vm_file_close(out_file);
        return 0;
    }

    for (unsigned int entry_index = 0; entry_index < entry_count; ++entry_index) {
        if (pos + 46u > (size_t)size_lo || read_le32(zip + pos) != 0x02014B50u) {
            copy_ascii(error_text, error_size, "Bad ZIP entry");
            vm_free(zip);
            vm_file_close(out_file);
            return 0;
        }

        unsigned short method = read_le16(zip + pos + 10);
        unsigned int compressed_size = read_le32(zip + pos + 20);
        unsigned int uncompressed_size = read_le32(zip + pos + 24);
        unsigned short name_len = read_le16(zip + pos + 28);
        unsigned short extra_len = read_le16(zip + pos + 30);
        unsigned short comment_len = read_le16(zip + pos + 32);
        unsigned int local_offset = read_le32(zip + pos + 42);
        const unsigned char *name = zip + pos + 46;

        if (pos + 46u + name_len + extra_len + comment_len > (size_t)size_lo) {
            copy_ascii(error_text, error_size, "ZIP name overflow");
            vm_free(zip);
            vm_file_close(out_file);
            return 0;
        }

        if (has_html_extension(name, name_len)) {
            found = 1;
            if (!process_zip_entry(zip, (size_t)size_lo, local_offset, method, compressed_size,
                                   uncompressed_size, &writer, error_text, error_size)) {
                vm_free(zip);
                vm_file_close(out_file);
                return 0;
            }
        }

        pos += 46u + name_len + extra_len + comment_len;
    }

    writer_flush(&writer);
    *truncated = writer.truncated;
    if (!found) {
        copy_ascii(error_text, error_size, "No HTML in EPUB");
        vm_free(zip);
        vm_file_close(out_file);
        return 0;
    }
    if (writer.io_error) {
        copy_ascii(error_text, error_size, "TXT write failed");
        vm_free(zip);
        vm_file_close(out_file);
        return 0;
    }

    vm_free(zip);
    vm_file_close(out_file);
    return 1;
}

static void start_conversion(void) {
    char error_text[80];
    int truncated = 0;

    g_state = STATE_CONVERTING;
    draw_screen();

    if (convert_epub_to_txt(error_text, sizeof(error_text), &truncated)) {
        if (truncated) {
            set_status("Done! (512KB cap)", kOutputPath);
        } else {
            set_status("Done!", kOutputPath);
        }
    } else {
        set_status("Error:", error_text);
    }

    g_state = STATE_DONE;
    draw_screen();
}

void vm_main(void) {
    layer_hdl[0] = -1;
    screen_w = vm_graphic_get_screen_width();
    screen_h = vm_graphic_get_screen_height();
    set_status("Ready", "");

    vm_reg_sysevt_callback(handle_sysevt);
    vm_reg_keyboard_callback(handle_keyevt);
    vm_reg_pen_callback(handle_penevt);
}

static void handle_sysevt(VMINT message, VMINT param) {
    (void)param;
#ifdef SUPPORT_BG
    switch (message) {
    case VM_MSG_CREATE:
        break;
    case VM_MSG_PAINT:
        ensure_layer();
        draw_screen();
        break;
    case VM_MSG_HIDE:
    case VM_MSG_QUIT:
        release_layer();
        break;
    }
#else
    switch (message) {
    case VM_MSG_CREATE:
    case VM_MSG_ACTIVE:
        ensure_layer();
        break;
    case VM_MSG_PAINT:
        ensure_layer();
        draw_screen();
        break;
    case VM_MSG_INACTIVE:
    case VM_MSG_QUIT:
        release_layer();
        break;
    }
#endif
}

static void handle_keyevt(VMINT event, VMINT keycode) {
    if (event != VM_KEY_EVENT_UP) {
        return;
    }

    //if (keycode == VM_KEY_RIGHT_SOFTKEY || keycode == VM_KEY_END) {
    if (keycode == VM_KEY_RIGHT_SOFTKEY || keycode == VM_KEY_NUM0) {
        release_layer();
        vm_exit_app();
        return;
    }

    if (g_state == STATE_READY) {
        //if (keycode == VM_KEY_LEFT_SOFTKEY || keycode == VM_KEY_SELECT) {
        if (keycode == VM_KEY_LEFT_SOFTKEY || keycode == VM_KEY_NUM1) {
            start_conversion();
        }
        return;
    }

    if (g_state == STATE_DONE) {
        g_state = STATE_READY;
        set_status("Ready", "");
        draw_screen();
    }
}

static void handle_penevt(VMINT event, VMINT x, VMINT y) {
    (void)event;
    (void)x;
    (void)y;
}
