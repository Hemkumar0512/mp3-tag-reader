/*
 * id3_parser.c
 * ------------
 * Implements reading and editing of ID3v2.2, ID3v2.3 and ID3v2.4 tags
 * in MP3 files.
 *
 * Background needed to follow this file:
 *
 *   An MP3 file that has an ID3v2 tag looks like:
 *
 *       [ 10-byte tag header ][ frames... ][ MPEG audio data ]
 *
 *   The header's size field says how many bytes of FRAMES follow (not
 *   counting the header itself). The 10-byte header is IDENTICAL across
 *   all three versions we support -- only its major_version byte (2, 3
 *   or 4) tells you which one you have.
 *
 *   The FRAMES, however, are laid out differently per version:
 *
 *     v2.2:  [3-byte ID][3-byte plain big-endian size][data...]
 *            (6-byte frame header, no flags)
 *     v2.3:  [4-byte ID][4-byte plain big-endian size][2-byte flags][data...]
 *     v2.4:  [4-byte ID][4-byte SYNCSAFE size]        [2-byte flags][data...]
 *
 *   Two "gotchas" this file has to deal with:
 *     1. Syncsafe integers: each byte only uses its lower 7 bits, top
 *        bit always 0. The tag header's size field is ALWAYS syncsafe,
 *        in every version. Frame sizes are syncsafe ONLY in v2.4.
 *     2. Everything is big-endian on disk. Most development machines
 *        (x86/x86-64) are little-endian, so multi-byte values must be
 *        assembled by hand with shifts, never read as a raw int.
 *
 *   Known limitations (documented rather than silently wrong):
 *     - All four ID3 text encodings (Latin-1, UTF-16 w/ BOM, UTF-16BE,
 *       UTF-8) are decoded and shown as UTF-8.
 *     - Unsynchronisation (tag-level or per-frame) and the v2.4 data
 *       length indicator are NOT undone. Ordinary tags from common
 *       taggers don't use them.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "id3_parser.h"

/* ==================================================================== */
/*  Small internal helpers (not exposed in the header on purpose --     */
/*  callers outside this file have no business calling these directly) */
/* ==================================================================== */

/*
 * Decodes a 4-byte SYNCSAFE integer. Used for the tag header's size
 * field (all versions) and for frame sizes in v2.4 only. Each byte
 * contributes 7 bits, so the maximum representable value is 2^28 - 1.
 */
static unsigned int decode_syncsafe(const unsigned char b[4])
{
    return ((unsigned int)(b[0] & 0x7F) << 21) |
           ((unsigned int)(b[1] & 0x7F) << 14) |
           ((unsigned int)(b[2] & 0x7F) << 7)  |
           ((unsigned int)(b[3] & 0x7F));
}

/* The reverse of decode_syncsafe(). */
static void encode_syncsafe(unsigned int size, unsigned char out[4])
{
    out[0] = (unsigned char)((size >> 21) & 0x7F);
    out[1] = (unsigned char)((size >> 14) & 0x7F);
    out[2] = (unsigned char)((size >> 7)  & 0x7F);
    out[3] = (unsigned char)(size & 0x7F);
}

/* Decodes a 4-byte ORDINARY big-endian integer. Used for frame sizes in
 * v2.3 (v2.2 uses a 3-byte version of this, see below; v2.4 uses
 * decode_syncsafe() instead). */
static unsigned int decode_uint32_be(const unsigned char b[4])
{
    return ((unsigned int)b[0] << 24) |
           ((unsigned int)b[1] << 16) |
           ((unsigned int)b[2] << 8)  |
           ((unsigned int)b[3]);
}

static void encode_uint32_be(unsigned int value, unsigned char out[4])
{
    out[0] = (unsigned char)((value >> 24) & 0xFF);
    out[1] = (unsigned char)((value >> 16) & 0xFF);
    out[2] = (unsigned char)((value >> 8)  & 0xFF);
    out[3] = (unsigned char)(value & 0xFF);
}

/* v2.2-only: frame sizes are a plain 24-bit (3-byte) big-endian integer. */
static unsigned int decode_uint24_be(const unsigned char b[3])
{
    return ((unsigned int)b[0] << 16) |
           ((unsigned int)b[1] << 8)  |
           ((unsigned int)b[2]);
}

static void encode_uint24_be(unsigned int value, unsigned char out[3])
{
    out[0] = (unsigned char)((value >> 16) & 0xFF);
    out[1] = (unsigned char)((value >> 8)  & 0xFF);
    out[2] = (unsigned char)(value & 0xFF);
}

/* True if this frame's ID matches either of two possible spellings --
 * used everywhere we need to accept both the v2.2 short ID (e.g. "TT2")
 * and the v2.3/v2.4 long ID (e.g. "TIT2") for the same logical field.
 * Pass NULL for 'short_id' if a field has no v2.2 equivalent. */
static int frame_id_is(const Frame *frame, const char *long_id, const char *short_id)
{
    if (strcmp(frame->frame_id, long_id) == 0)
    {
        return 1;
    }
    if (short_id != NULL && strcmp(frame->frame_id, short_id) == 0)
    {
        return 1;
    }
    return 0;
}

/* ---------- Text-encoding support ------------------------------------
 *
 * ID3 text frames start with an "encoding" byte:
 *   0x00 = ISO-8859-1 (Latin-1)
 *   0x01 = UTF-16 with a byte-order mark  (v2.2/v2.3/v2.4)
 *   0x02 = UTF-16BE, no BOM               (v2.4 only)
 *   0x03 = UTF-8                          (v2.4 only)
 * Everything read from a tag is converted to UTF-8 in the TagInfo
 * strings, so the terminal always gets one consistent encoding.
 */
#define ENC_LATIN1   0x00
#define ENC_UTF16    0x01
#define ENC_UTF16BE  0x02
#define ENC_UTF8     0x03

/* Appends one code point to dest as UTF-8, only if the WHOLE character
 * fits (so truncation never cuts a multi-byte sequence in half).
 * Returns 1 on success, 0 if there was no room. */
static int put_codepoint(char *dest, size_t dest_size, size_t *len, unsigned int cp)
{
    unsigned char buf[4];
    size_t n;

    if (cp < 0x80)         { buf[0] = (unsigned char)cp; n = 1; }
    else if (cp < 0x800)   { buf[0] = (unsigned char)(0xC0 | (cp >> 6));
                             buf[1] = (unsigned char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) { buf[0] = (unsigned char)(0xE0 | (cp >> 12));
                             buf[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
                             buf[2] = (unsigned char)(0x80 | (cp & 0x3F)); n = 3; }
    else                   { buf[0] = (unsigned char)(0xF0 | (cp >> 18));
                             buf[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
                             buf[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
                             buf[3] = (unsigned char)(0x80 | (cp & 0x3F)); n = 4; }

    if (*len + n + 1 > dest_size) /* +1 keeps room for '\0' */
    {
        return 0;
    }
    memcpy(dest + *len, buf, n);
    *len += n;
    return 1;
}

/* Reads one UTF-8 code point from s[*i], advancing *i. Malformed input
 * yields U+FFFD and advances one byte. Used when WRITING tags. */
static unsigned int utf8_next(const unsigned char *s, size_t len, size_t *i)
{
    unsigned char c = s[*i];
    size_t need;
    unsigned int cp;

    if (c < 0x80) { (*i)++; return c; }
    if      ((c & 0xE0) == 0xC0) { need = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { need = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { need = 3; cp = c & 0x07; }
    else { (*i)++; return 0xFFFD; }

    if (*i + need >= len) { (*i)++; return 0xFFFD; }
    for (size_t k = 1; k <= need; k++)
    {
        if ((s[*i + k] & 0xC0) != 0x80) { (*i)++; return 0xFFFD; }
        cp = (cp << 6) | (s[*i + k] & 0x3F);
    }
    *i += need + 1;
    return cp;
}

/*
 * Decodes ONE terminated string from data[0..len) (encoding 'enc') and
 * APPENDS it to dest as UTF-8, starting at dest[*out_len]. Stops at the
 * string's terminator (NUL, or 00 00 for UTF-16). Returns the number of
 * source bytes consumed INCLUDING the terminator, so callers can carry
 * on to the next string in a multi-value frame.
 */
static size_t decode_text(char *dest, size_t dest_size, size_t *out_len,
                          const unsigned char *data, size_t len, unsigned char enc)
{
    size_t i = 0;
    int full = 0;

    if (enc == ENC_UTF16 || enc == ENC_UTF16BE)
    {
        int big_endian = (enc == ENC_UTF16BE);

        /* A BOM (FF FE = little-endian, FE FF = big-endian) overrides
         * the default. Without one, UTF-16 (0x01) is assumed LE. */
        if (len >= 2 && data[0] == 0xFF && data[1] == 0xFE)      { big_endian = 0; i = 2; }
        else if (len >= 2 && data[0] == 0xFE && data[1] == 0xFF) { big_endian = 1; i = 2; }

        while (i + 1 < len)
        {
            unsigned int u = big_endian ? (unsigned)((data[i] << 8) | data[i + 1])
                                        : (unsigned)((data[i + 1] << 8) | data[i]);
            i += 2;
            if (u == 0)
            {
                return i; /* terminator */
            }

            unsigned int cp = u;
            if (u >= 0xD800 && u < 0xDC00 && i + 1 < len)
            {
                unsigned int lo = big_endian ? (unsigned)((data[i] << 8) | data[i + 1])
                                             : (unsigned)((data[i + 1] << 8) | data[i]);
                if (lo >= 0xDC00 && lo < 0xE000)
                {
                    cp = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                    i += 2;
                }
                else { cp = 0xFFFD; }
            }
            else if (u >= 0xD800 && u < 0xE000) { cp = 0xFFFD; } /* lone surrogate */

            if (!full && !put_codepoint(dest, dest_size, out_len, cp)) { full = 1; }
        }
        return len;
    }

    while (i < len && data[i] != 0x00)
    {
        if (!full)
        {
            if (enc == ENC_UTF8)
            {
                /* Already UTF-8: copy the byte, but never leave a
                 * half-written multi-byte character at the cut-off. */
                if (*out_len + 1 + 1 > dest_size) { full = 1; }
                else
                {
                    unsigned char c = data[i];
                    size_t seq = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
                    if (seq > 1 && *out_len + seq + 1 > dest_size) { full = 1; }
                    else
                    {
                        size_t take = (i + seq <= len) ? seq : 1;
                        for (size_t k = 0; k < take; k++) { dest[(*out_len)++] = (char)data[i + k]; }
                        i += take;
                        continue;
                    }
                }
            }
            else if (!put_codepoint(dest, dest_size, out_len, data[i])) /* Latin-1 */
            {
                full = 1;
            }
        }
        i++;
    }
    return (i < len) ? i + 1 : len; /* +1 steps over the NUL */
}

/* Returns how many bytes to skip to get past ONE terminated string
 * (NUL, or an aligned 00 00 for UTF-16). Used to hop over the short
 * description in COMM / APIC / PIC frames. */
static size_t skip_terminated(const unsigned char *data, size_t len, unsigned char enc)
{
    size_t i = 0;
    if (enc == ENC_UTF16 || enc == ENC_UTF16BE)
    {
        while (i + 1 < len)
        {
            if (data[i] == 0x00 && data[i + 1] == 0x00) { return i + 2; }
            i += 2;
        }
        return len;
    }
    while (i < len && data[i] != 0x00) { i++; }
    return (i < len) ? i + 1 : len;
}

/*
 * Copies a TEXT frame into a bounded UTF-8 buffer.
 *
 * v2.4 allows several NUL-separated values in one frame (e.g. two
 * artists); they are joined with " / " so none are silently lost.
 */
static void copy_text_frame(char *dest, size_t dest_size, const Frame *frame)
{
    dest[0] = '\0';
    if (frame->size == 0)
    {
        return;
    }

    unsigned char enc = frame->actual_data[0];
    if (enc > ENC_UTF8)
    {
        snprintf(dest, dest_size, "[unsupported text encoding: 0x%02X]", enc);
        return;
    }

    size_t pos = 1, out = 0;
    while (pos < frame->size)
    {
        size_t before = out;
        if (out > 0)
        {
            if (out + 3 + 1 > dest_size) { break; }
            memcpy(dest + out, " / ", 3);
            out += 3;
        }
        size_t val_start = out;
        pos += decode_text(dest, dest_size, &out, frame->actual_data + pos,
                           frame->size - pos, enc);
        if (out == val_start)
        {
            out = before; /* empty value (e.g. trailing NUL): drop it */
        }
    }
    dest[out] = '\0';
}

/*
 * COMM (v2.3/v2.4) / COM (v2.2):
 *   [encoding][3-byte language][short description, terminated][text]
 * Only the text is surfaced. The description's terminator depends on
 * the encoding (00 for Latin-1/UTF-8, 00 00 for UTF-16).
 */
static void copy_comment_frame(char *dest, size_t dest_size, const Frame *frame)
{
    dest[0] = '\0';
    if (frame->size < 4)
    {
        return;
    }

    unsigned char enc = frame->actual_data[0];
    if (enc > ENC_UTF8)
    {
        snprintf(dest, dest_size, "[unsupported text encoding: 0x%02X]", enc);
        return;
    }

    size_t pos = 4; /* past encoding + language */
    pos += skip_terminated(frame->actual_data + pos, frame->size - pos, enc);
    if (pos >= frame->size)
    {
        return;
    }

    size_t out = 0;
    decode_text(dest, dest_size, &out, frame->actual_data + pos, frame->size - pos, enc);
    dest[out] = '\0';
}

/*
 * APIC (v2.3/v2.4):
 *   [encoding][MIME, NUL-terminated (always Latin-1)][picture type]
 *   [description, terminated per encoding][image bytes...]
 * Only the FIRST picture is kept (a second one would leak the first
 * one's buffer).
 */
static void extract_apic_frame(TagInfo *info, const Frame *frame)
{
    if (info->has_art || frame->size < 2)
    {
        return;
    }

    unsigned char enc = frame->actual_data[0];
    size_t pos = 1;

    size_t mime_start = pos;
    while (pos < frame->size && frame->actual_data[pos] != '\0')
    {
        pos++;
    }
    size_t mime_len = pos - mime_start;
    if (mime_len >= MAX_MIME)
    {
        mime_len = MAX_MIME - 1;
    }
    memcpy(info->art_mime, frame->actual_data + mime_start, mime_len);
    info->art_mime[mime_len] = '\0';
    pos++; /* MIME terminator */

    if (pos >= frame->size)
    {
        return;
    }

    info->art_type = frame->actual_data[pos];
    pos++;

    pos += skip_terminated(frame->actual_data + pos, frame->size - pos, enc);
    if (pos >= frame->size)
    {
        return; /* no image bytes followed */
    }

    unsigned int image_len = frame->size - (unsigned int)pos;
    unsigned char *image_copy = malloc(image_len);
    if (image_copy == NULL)
    {
        return;
    }
    memcpy(image_copy, frame->actual_data + pos, image_len);

    info->has_art  = 1;
    info->art_size = image_len;
    info->art_data = image_copy;
}

/*
 * PIC (v2.2): [encoding][3-byte image format][picture type]
 *             [description, terminated per encoding][image bytes...]
 */
static void extract_pic_frame_v22(TagInfo *info, const Frame *frame)
{
    if (info->has_art || frame->size < 5)
    {
        return;
    }

    unsigned char enc = frame->actual_data[0];
    size_t pos = 1;

    memcpy(info->art_mime, frame->actual_data + pos, 3);
    info->art_mime[3] = '\0'; /* e.g. "JPG" */
    pos += 3;

    info->art_type = frame->actual_data[pos];
    pos++;

    pos += skip_terminated(frame->actual_data + pos, frame->size - pos, enc);
    if (pos >= frame->size)
    {
        return;
    }

    unsigned int image_len = frame->size - (unsigned int)pos;
    unsigned char *image_copy = malloc(image_len);
    if (image_copy == NULL)
    {
        return;
    }
    memcpy(image_copy, frame->actual_data + pos, image_len);

    info->has_art  = 1;
    info->art_size = image_len;
    info->art_data = image_copy;
}

/*
 * Routes one decoded frame into the right TagInfo field (v2.2 short IDs
 * and v2.3/v2.4 long IDs). Unrecognised frames are ignored.
 *
 * Year: v2.4 replaced TYER with TDRC ("recording time", e.g.
 * "2021-05-03"; the 5-byte year buffer keeps the first 4 characters).
 * TDRC wins if both are present; some taggers still write TYER in v2.4.
 */
static void extract_field(TagInfo *info, const Frame *frame)
{
    if      (frame_id_is(frame, "TIT2", "TT2")) copy_text_frame(info->title,  sizeof(info->title),  frame);
    else if (frame_id_is(frame, "TPE1", "TP1")) copy_text_frame(info->artist, sizeof(info->artist), frame);
    else if (frame_id_is(frame, "TALB", "TAL")) copy_text_frame(info->album,  sizeof(info->album),  frame);
    else if (frame_id_is(frame, "TDRC", NULL))  copy_text_frame(info->year,   sizeof(info->year),   frame);
    else if (frame_id_is(frame, "TYER", "TYE"))
    {
        if (info->year[0] == '\0') copy_text_frame(info->year, sizeof(info->year), frame);
    }
    else if (frame_id_is(frame, "TRCK", "TRK")) copy_text_frame(info->track,  sizeof(info->track),  frame);
    else if (frame_id_is(frame, "TCON", "TCO")) copy_text_frame(info->genre,  sizeof(info->genre),  frame);
    else if (frame_id_is(frame, "COMM", "COM"))
    {
        /* Files often carry several COMM frames (iTunes adds hidden
         * ones); keep the first non-empty one instead of the last. */
        if (info->comment[0] == '\0') copy_comment_frame(info->comment, sizeof(info->comment), frame);
    }
    else if (strcmp(frame->frame_id, "APIC") == 0) extract_apic_frame(info, frame);
    else if (strcmp(frame->frame_id, "PIC")  == 0) extract_pic_frame_v22(info, frame);
}

/*
 * Builds the data bytes of a frame we are WRITING (edit mode).
 *   plain text : [enc][text]
 *   comment    : [enc]["eng"][empty description + terminator][text]
 * Encoding choice: pure ASCII -> Latin-1 (max compatibility); otherwise
 * UTF-8 for v2.4, or UTF-16+BOM for v2.2/v2.3 (which have no UTF-8).
 * Returns a malloc'd buffer (caller frees) or NULL on out-of-memory.
 */
static unsigned char *build_frame_data(unsigned char major, int is_comment,
                                       const char *value, unsigned int *out_size)
{
    const unsigned char *v = (const unsigned char *)value;
    size_t len = strlen(value);

    int ascii = 1;
    for (size_t i = 0; i < len; i++)
    {
        if (v[i] >= 0x80) { ascii = 0; break; }
    }
    unsigned char enc = ascii ? ENC_LATIN1 : (major == 4 ? ENC_UTF8 : ENC_UTF16);
    int wide = (enc == ENC_UTF16);

    unsigned char *buf = malloc(1 + 3 + 4 + 2 + len * 2 + 1);
    if (buf == NULL)
    {
        return NULL;
    }

    size_t n = 0;
    buf[n++] = enc;

    if (is_comment)
    {
        memcpy(buf + n, "eng", 3);
        n += 3;
        if (wide) { buf[n++] = 0xFF; buf[n++] = 0xFE; buf[n++] = 0; buf[n++] = 0; }
        else      { buf[n++] = 0; }
    }

    if (wide)
    {
        buf[n++] = 0xFF; buf[n++] = 0xFE; /* BOM: little-endian */
        size_t i = 0;
        while (i < len)
        {
            unsigned int cp = utf8_next(v, len, &i);
            if (cp >= 0x10000)
            {
                cp -= 0x10000;
                unsigned int hi = 0xD800 | (cp >> 10), lo = 0xDC00 | (cp & 0x3FF);
                buf[n++] = (unsigned char)(hi & 0xFF); buf[n++] = (unsigned char)(hi >> 8);
                buf[n++] = (unsigned char)(lo & 0xFF); buf[n++] = (unsigned char)(lo >> 8);
            }
            else
            {
                buf[n++] = (unsigned char)(cp & 0xFF); buf[n++] = (unsigned char)(cp >> 8);
            }
        }
    }
    else
    {
        memcpy(buf + n, v, len);
        n += len;
    }

    *out_size = (unsigned int)n;
    return buf;
}

/*
 * Maps a -t/-T/-a/-A/-y/-c/-g style modifier character to the frame ID
 * appropriate for the given tag version: the short 3-character form for
 * v2.2, the long 4-character form for v2.3/v2.4. Returns NULL for an
 * unrecognised modifier.
 */
static const char *field_to_frame_id(char field, unsigned char major_version)
{
    if (major_version == 2)
    {
        switch (field)
        {
            case 't': return "TT2";
            case 'T': return "TRK";
            case 'a': return "TP1";
            case 'A': return "TAL";
            case 'y': return "TYE";
            case 'c': return "COM";
            case 'g': return "TCO";
            default:  return NULL;
        }
    }

    if (major_version == 4 && field == 'y')
    {
        return "TDRC"; /* v2.4 has no TYER */
    }

    switch (field)
    {
        case 't': return "TIT2";
        case 'T': return "TRCK";
        case 'a': return "TPE1";
        case 'A': return "TALB";
        case 'y': return "TYER";
        case 'c': return "COMM";
        case 'g': return "TCON";
        default:  return NULL;
    }
}

/* Is this frame ID (of whatever length) the comment frame for its
 * version? Used by the editor to decide how to build replacement data. */
static int is_comment_frame_id(const char *frame_id)
{
    return strcmp(frame_id, "COMM") == 0 || strcmp(frame_id, "COM") == 0;
}

/* Frame-header size on disk for a given version: 6 bytes for v2.2
 * (3-byte ID + 3-byte size, no flags), 10 bytes for v2.3/v2.4
 * (4-byte ID + 4-byte size + 2-byte flags). */
static int frame_header_size(unsigned char major_version)
{
    return (major_version == 2) ? 6 : 10;
}

/* Where the audio starts in the ORIGINAL file: 10-byte header + tag body
 * (which includes any padding) + the 10-byte footer if a v2.4 tag has one. */
static long audio_start_offset(const Header *hdr)
{
    long off = 10 + (long)hdr->size;
    if (hdr->major_version == 4 && (hdr->flags & 0x10))
    {
        off += 10;
    }
    return off;
}

/* Finds the frame for a modifier. In v2.4 the year is TDRC, but files
 * written by sloppy taggers may still carry TYER, so fall back to it. */
static int find_field_frame(const Frame *frames, int count, const char *target_id,
                            char field, unsigned char major)
{
    for (int i = 0; i < count; i++)
    {
        if (strcmp(frames[i].frame_id, target_id) == 0) return i;
    }
    if (major == 4 && field == 'y')
    {
        for (int i = 0; i < count; i++)
        {
            if (strcmp(frames[i].frame_id, "TYER") == 0) return i;
        }
    }
    return -1;
}

/* Puts the rewritten temp file in place of the original. rename() alone
 * replaces atomically on POSIX; the remove()+rename() fallback covers
 * platforms where rename refuses to overwrite. */
static int replace_file(const char *temp, const char *dest)
{
    if (rename(temp, dest) == 0)
    {
        return 0;
    }
    if (remove(dest) != 0)
    {
        remove(temp);
        return -1;
    }
    return (rename(temp, dest) == 0) ? 0 : -1;
}

/* Skips the v2.3/v2.4 extended header if the tag has one (flag bit 6),
 * so frame parsing starts at the first real frame. *consumed is how many
 * of the tag's bytes it occupied. */
static Status skip_extended_header(FILE *fp, const Header *hdr, unsigned int *consumed)
{
    *consumed = 0;
    if (hdr->major_version < 3 || !(hdr->flags & 0x40))
    {
        return ID3_OK;
    }

    unsigned char b[4];
    if (fread(b, 1, 4, fp) != 4)
    {
        return ID3_ERR_READ;
    }
    /* v2.4: syncsafe size INCLUDING these 4 bytes; v2.3: plain size EXCLUDING them. */
    unsigned int total = (hdr->major_version == 4) ? decode_syncsafe(b)
                                                    : decode_uint32_be(b) + 4;
    if (total < 4 || total > hdr->size ||
        fseek(fp, (long)(total - 4), SEEK_CUR) != 0)
    {
        return ID3_ERR_READ;
    }
    *consumed = total;
    return ID3_OK;
}

/* ==================================================================== */
/*  Public API -- validation, reading                                   */
/* ==================================================================== */

Status check_mp3_extension(const char *filename)
{
    if (filename == NULL)
    {
        return ID3_ERR_ARGS;
    }

    size_t len = strlen(filename);
    const char *ext = ".mp3";
    size_t ext_len = strlen(ext);

    if (len < ext_len)
    {
        return ID3_ERR_NOT_MP3;
    }

    if (strcmp(filename + (len - ext_len), ext) != 0)
    {
        return ID3_ERR_NOT_MP3;
    }

    return ID3_OK;
}

Status read_header(FILE *fp, Header *hdr)
{
    if (fp == NULL || hdr == NULL)
    {
        return ID3_ERR_ARGS;
    }

    /* Always start from byte 0: the header never depends on wherever
     * the caller happened to leave the file position. */
    if (fseek(fp, 0, SEEK_SET) != 0)
    {
        return ID3_ERR_READ;
    }

    /* --- Identifier: 3 bytes, should be "ID3" --- */
    if (fread(hdr->identifier, 1, 3, fp) != 3)
    {
        return ID3_ERR_NO_TAG; /* file too short to even hold an identifier */
    }
    hdr->identifier[3] = '\0';

    if (strcmp(hdr->identifier, "ID3") != 0)
    {
        return ID3_ERR_NO_TAG;
    }

    /* --- Version, revision, flags: one byte each --- */
    if (fread(&hdr->major_version, 1, 1, fp) != 1 ||
        fread(&hdr->revision,      1, 1, fp) != 1 ||
        fread(&hdr->flags,         1, 1, fp) != 1)
    {
        return ID3_ERR_READ;
    }

    /* v2.2, v2.3 and v2.4 are all supported. Anything else (there is no
     * such thing as v2.0/v2.1/v2.5+ in practice) is unsupported. */
    if (hdr->major_version != 2 && hdr->major_version != 3 && hdr->major_version != 4)
    {
        return ID3_ERR_UNSUPPORTED;
    }

    /* --- Size: 4 syncsafe bytes -- ALWAYS syncsafe, in every version --- */
    unsigned char size_bytes[4];
    if (fread(size_bytes, 1, 4, fp) != 4)
    {
        return ID3_ERR_READ;
    }
    hdr->size = decode_syncsafe(size_bytes);

    return ID3_OK;
}

Status read_frame(FILE *fp, unsigned char major_version, Frame *frame)
{
    if (fp == NULL || frame == NULL)
    {
        return ID3_ERR_ARGS;
    }
    frame->actual_data = NULL;

    if (major_version == 2)
    {
        /* --- v2.2: 3-byte ID, 3-byte plain size, no flags --- */
        unsigned char id_bytes[3];
        if (fread(id_bytes, 1, 3, fp) != 3)
        {
            return ID3_END_OF_FRAMES; /* ran off the end of the file */
        }
        if (id_bytes[0] == 0x00)
        {
            return ID3_END_OF_FRAMES; /* padding */
        }
        memcpy(frame->frame_id, id_bytes, 3);
        frame->frame_id[3] = '\0';

        unsigned char size_bytes[3];
        if (fread(size_bytes, 1, 3, fp) != 3)
        {
            return ID3_ERR_READ;
        }
        frame->size = decode_uint24_be(size_bytes);
        frame->flags[0] = 0;
        frame->flags[1] = 0;
    }
    else
    {
        /* --- v2.3/v2.4: 4-byte ID, 4-byte size, 2-byte flags --- */
        unsigned char id_bytes[4];
        if (fread(id_bytes, 1, 4, fp) != 4)
        {
            return ID3_END_OF_FRAMES;
        }
        if (id_bytes[0] == 0x00)
        {
            return ID3_END_OF_FRAMES;
        }
        memcpy(frame->frame_id, id_bytes, 4);
        frame->frame_id[4] = '\0';

        unsigned char size_bytes[4];
        if (fread(size_bytes, 1, 4, fp) != 4)
        {
            return ID3_ERR_READ;
        }
        /* This next line is the one place v2.3 and v2.4 genuinely
         * differ in how a frame is read: v2.4 sizes are syncsafe,
         * v2.3 sizes are plain big-endian. */
        frame->size = (major_version == 4) ? decode_syncsafe(size_bytes)
                                            : decode_uint32_be(size_bytes);

        if (fread(frame->flags, 1, 2, fp) != 2)
        {
            return ID3_ERR_READ;
        }
    }

    if (frame->size == 0)
    {
        return ID3_OK;
    }

    /* A corrupt size field must not make us malloc gigabytes: the frame
     * can't be larger than what is left in the file. */
    long here = ftell(fp);
    if (here < 0 || fseek(fp, 0, SEEK_END) != 0)
    {
        return ID3_ERR_READ;
    }
    long file_end = ftell(fp);
    if (fseek(fp, here, SEEK_SET) != 0 || file_end < here ||
        (unsigned long)(file_end - here) < frame->size)
    {
        return ID3_ERR_READ;
    }

    frame->actual_data = malloc(frame->size);
    if (frame->actual_data == NULL)
    {
        return ID3_ERR_NOMEM;
    }

    if (fread(frame->actual_data, 1, frame->size, fp) != frame->size)
    {
        free(frame->actual_data);
        frame->actual_data = NULL;
        return ID3_ERR_READ;
    }

    return ID3_OK;
}

void free_frame(Frame *frame)
{
    if (frame == NULL)
    {
        return;
    }
    free(frame->actual_data);
    frame->actual_data = NULL; /* guards against an accidental double free */
}

/*
 * Reads every frame of the tag described by 'hdr' into a caller-supplied
 * array. Shared by parse_tag(), modify_tag_field() and
 * delete_tag_field() so the "loop until padding or tag size" logic
 * exists in exactly one place.
 *
 * fp must already have had read_header() called on it (that is where
 * hdr came from); this function reads starting from wherever the file
 * position currently is, i.e. right after the header.
 */
static Status load_frames(FILE *fp, const Header *hdr, Frame *frames,
                           int max_frames, int *out_count)
{
    *out_count = 0;
    unsigned int bytes_consumed = 0;
    int hdr_size = frame_header_size(hdr->major_version);

    Status status = skip_extended_header(fp, hdr, &bytes_consumed);
    if (status != ID3_OK)
    {
        return status;
    }

    while (bytes_consumed < hdr->size && *out_count < max_frames)
    {
        Frame frame;
        status = read_frame(fp, hdr->major_version, &frame);

        if (status == ID3_END_OF_FRAMES)
        {
            break;
        }
        if (status != ID3_OK)
        {
            for (int i = 0; i < *out_count; i++)
            {
                free_frame(&frames[i]);
            }
            *out_count = 0;
            return status;
        }

        bytes_consumed += (unsigned int)hdr_size + frame.size;
        frames[(*out_count)++] = frame;
    }

    /* If the array filled up and real frames (not padding) remain, a
     * rewrite would silently DROP them. Refuse instead of losing data. */
    if (*out_count == max_frames && bytes_consumed < hdr->size)
    {
        int next = fgetc(fp);
        if (next != EOF && next != 0x00)
        {
            for (int i = 0; i < *out_count; i++)
            {
                free_frame(&frames[i]);
            }
            *out_count = 0;
            return ID3_ERR_NOMEM;
        }
    }

    return ID3_OK;
}

Status parse_tag(const char *filename, TagInfo *info)
{
    if (filename == NULL || info == NULL)
    {
        return ID3_ERR_ARGS;
    }

    /* Zero everything first: this is what makes "empty string == field
     * absent" a safe assumption everywhere else in the program. */
    memset(info, 0, sizeof(*info));

    FILE *fp = fopen(filename, "rb"); /* "b" matters: this is binary data */
    if (fp == NULL)
    {
        return ID3_ERR_OPEN;
    }

    Status status = read_header(fp, &info->header);
    if (status != ID3_OK)
    {
        fclose(fp);
        return status;
    }

    unsigned int bytes_consumed = 0;
    int hdr_size = frame_header_size(info->header.major_version);

    status = skip_extended_header(fp, &info->header, &bytes_consumed);
    if (status != ID3_OK)
    {
        fclose(fp);
        return status;
    }

    while (bytes_consumed < info->header.size)
    {
        Frame frame;
        status = read_frame(fp, info->header.major_version, &frame);

        if (status == ID3_END_OF_FRAMES)
        {
            break;
        }
        if (status != ID3_OK)
        {
            fclose(fp);
            free_tag_info(info); /* don't leak album art read so far */
            return status;
        }

        bytes_consumed += (unsigned int)hdr_size + frame.size;
        extract_field(info, &frame);
        free_frame(&frame);
    }

    fclose(fp);
    return ID3_OK;
}

void free_tag_info(TagInfo *info)
{
    if (info == NULL)
    {
        return;
    }
    free(info->art_data);
    info->art_data = NULL;
    info->has_art = 0;
}

void print_tag(const TagInfo *info)
{
    printf("Mp3 Tag Reader & Editor:\n");
    printf("-------------------------\n");
    /* The header's major_version byte IS the number after "2." -- for
     * an ID3v2.3 tag, major_version == 3, and the spec's sample output
     * shows this as "Version ID : 2.3". */
    printf("Version ID : 2.%d\n", info->header.major_version);
    printf("Title : %s\n", info->title);
    printf("Album : %s\n", info->album);
    printf("Year : %s\n", info->year);
    printf("Track : %s\n", info->track);
    printf("Genre : %s\n", info->genre);
    printf("Artist : %s\n", info->artist);
    printf("Comment : %s\n", info->comment);
    /* Album art is NOT printed here -- main.c prints its details (type,
     * size) and handles extraction, since only main.c knows the output
     * file path being written to. */
}

const char *field_name(char field)
{
    switch (field)
    {
        case 't': return "Title";
        case 'T': return "Track";
        case 'a': return "Artist";
        case 'A': return "Album";
        case 'y': return "Year";
        case 'c': return "Comment";
        case 'g': return "Genre";
        default:  return NULL;
    }
}

/* ==================================================================== */
/*  Public API -- writing (editing and deleting)                        */
/* ==================================================================== */

#define MAX_FRAMES 256

/*
 * Writes a complete new tag (header + all frames) to 'out', in the
 * layout appropriate for major_version. Shared by modify_tag_field()
 * and delete_tag_field() -- delete_all_tags() does not call this, since
 * it writes no tag at all.
 */
static Status write_tag(FILE *out, unsigned char major_version,
                         Frame *frames, int frame_count)
{
    int hdr_size = frame_header_size(major_version);
    unsigned int new_tag_size = 0;
    for (int i = 0; i < frame_count; i++)
    {
        new_tag_size += (unsigned int)hdr_size + frames[i].size;
    }

    unsigned char size_bytes[4];
    encode_syncsafe(new_tag_size, size_bytes); /* header size is ALWAYS syncsafe */

    if (fwrite("ID3", 1, 3, out) != 3 ||
        fputc(major_version, out) == EOF ||
        fputc(0, out) == EOF ||             /* revision                 */
        fputc(0, out) == EOF ||             /* flags                    */
        fwrite(size_bytes, 1, 4, out) != 4)
    {
        return ID3_ERR_WRITE;
    }

    for (int i = 0; i < frame_count; i++)
    {
        if (major_version == 2)
        {
            unsigned char frame_size_bytes[3];
            encode_uint24_be(frames[i].size, frame_size_bytes);

            if (fwrite(frames[i].frame_id, 1, 3, out) != 3 ||
                fwrite(frame_size_bytes, 1, 3, out) != 3 ||
                (frames[i].size > 0 &&
                 fwrite(frames[i].actual_data, 1, frames[i].size, out) != frames[i].size))
            {
                return ID3_ERR_WRITE;
            }
        }
        else
        {
            unsigned char frame_size_bytes[4];
            if (major_version == 4)
            {
                encode_syncsafe(frames[i].size, frame_size_bytes);
            }
            else
            {
                encode_uint32_be(frames[i].size, frame_size_bytes);
            }

            if (fwrite(frames[i].frame_id, 1, 4, out) != 4 ||
                fwrite(frame_size_bytes, 1, 4, out) != 4 ||
                fwrite(frames[i].flags, 1, 2, out) != 2 ||
                (frames[i].size > 0 &&
                 fwrite(frames[i].actual_data, 1, frames[i].size, out) != frames[i].size))
            {
                return ID3_ERR_WRITE;
            }
        }
    }

    return ID3_OK;
}

/* Copies every remaining byte of 'src' (from its current file position)
 * to 'dst'. Used to carry the original audio data across into the
 * rewritten file, unchanged, in all three of modify/delete/delete-all. */
static Status copy_remaining_bytes(FILE *src, FILE *dst)
{
    unsigned char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0)
    {
        if (fwrite(buf, 1, n, dst) != n)
        {
            return ID3_ERR_WRITE;
        }
    }
    return ID3_OK;
}

static void free_all_frames(Frame *frames, int count)
{
    for (int i = 0; i < count; i++)
    {
        free_frame(&frames[i]);
    }
}

/*
 * modify_tag_field() -- the editor.
 *
 * Changing a tag value almost always changes that frame's SIZE, which
 * changes the tag's total size, which changes where the audio data
 * starts. There is no way to edit "in place" in the general case, so
 * this function:
 *
 *   1. Reads the whole tag into memory as an array of Frame structs.
 *   2. Finds the frame matching the requested field (in the file's own
 *      version's frame-ID spelling) and replaces its data, or creates a
 *      new frame if that field was not present.
 *   3. Writes a brand new file: new header, all frames, then the
 *      original audio data copied byte-for-byte.
 *   4. Replaces the original file with the new one.
 *
 * The tag's original major_version (2, 3, or 4) is preserved -- editing
 * never changes which ID3 version a file uses.
 */
Status modify_tag_field(const char *filename, char field, const char *new_value)
{
    if (filename == NULL || new_value == NULL)
    {
        return ID3_ERR_ARGS;
    }

    FILE *fp = fopen(filename, "rb");
    if (fp == NULL)
    {
        return ID3_ERR_OPEN;
    }

    Header hdr;
    Status status = read_header(fp, &hdr);
    if (status != ID3_OK)
    {
        fclose(fp);
        return status;
    }

    const char *target_id = field_to_frame_id(field, hdr.major_version);
    if (target_id == NULL)
    {
        fclose(fp);
        return ID3_ERR_BAD_MODIFIER;
    }

    Frame frames[MAX_FRAMES];
    int frame_count = 0;
    status = load_frames(fp, &hdr, frames, MAX_FRAMES, &frame_count);
    if (status != ID3_OK)
    {
        fclose(fp);
        return status;
    }

    /* Audio data in the ORIGINAL file starts right after the original
     * tag -- 10-byte header, always, regardless of version. */
    long audio_offset = audio_start_offset(&hdr);

    int target_index = find_field_frame(frames, frame_count, target_id, field,
                                        hdr.major_version);

    /* Build the new frame data (encoding chosen per tag version). */
    unsigned int   new_size = 0;
    unsigned char *new_data = build_frame_data(hdr.major_version,
                                               is_comment_frame_id(target_id),
                                               new_value, &new_size);
    if (new_data == NULL)
    {
        free_all_frames(frames, frame_count);
        fclose(fp);
        return ID3_ERR_NOMEM;
    }

    if (target_index >= 0)
    {
        free(frames[target_index].actual_data);
        frames[target_index].actual_data = new_data;
        frames[target_index].size = new_size;
        /* Old flags may mean compressed/encrypted/unsynchronised/data
         * length indicator -- none of which apply to our fresh data. */
        frames[target_index].flags[0] = 0;
        frames[target_index].flags[1] = 0;
    }
    else
    {
        if (frame_count >= MAX_FRAMES)
        {
            free(new_data);
            free_all_frames(frames, frame_count);
            fclose(fp);
            return ID3_ERR_NOMEM; /* out of frame slots */
        }
        strcpy(frames[frame_count].frame_id, target_id);
        frames[frame_count].flags[0] = 0;
        frames[frame_count].flags[1] = 0;
        frames[frame_count].actual_data = new_data;
        frames[frame_count].size = new_size;
        frame_count++;
    }

    char temp_filename[1024];
    snprintf(temp_filename, sizeof(temp_filename), "%s.tmp", filename);

    FILE *out = fopen(temp_filename, "wb");
    if (out == NULL)
    {
        free_all_frames(frames, frame_count);
        fclose(fp);
        return ID3_ERR_OPEN;
    }

    status = write_tag(out, hdr.major_version, frames, frame_count);
    if (status == ID3_OK && fseek(fp, audio_offset, SEEK_SET) == 0)
    {
        status = copy_remaining_bytes(fp, out);
    }
    else if (status == ID3_OK)
    {
        status = ID3_ERR_READ;
    }

    free_all_frames(frames, frame_count);
    fclose(fp);
    fclose(out);

    if (status != ID3_OK)
    {
        remove(temp_filename);
        return status;
    }

    if (replace_file(temp_filename, filename) != 0)
    {
        return ID3_ERR_WRITE;
    }

    return ID3_OK;
}

/*
 * delete_tag_field() -- additional feature: remove one frame.
 *
 * Same read-modify-write shape as modify_tag_field(), except instead of
 * replacing the target frame's data, we drop it from the array entirely
 * (by copying every later frame one slot back over it) and write out
 * whatever remains.
 */
Status delete_tag_field(const char *filename, char field)
{
    if (filename == NULL)
    {
        return ID3_ERR_ARGS;
    }

    FILE *fp = fopen(filename, "rb");
    if (fp == NULL)
    {
        return ID3_ERR_OPEN;
    }

    Header hdr;
    Status status = read_header(fp, &hdr);
    if (status != ID3_OK)
    {
        fclose(fp);
        return status;
    }

    const char *target_id = field_to_frame_id(field, hdr.major_version);
    if (target_id == NULL)
    {
        fclose(fp);
        return ID3_ERR_BAD_MODIFIER;
    }

    Frame frames[MAX_FRAMES];
    int frame_count = 0;
    status = load_frames(fp, &hdr, frames, MAX_FRAMES, &frame_count);
    if (status != ID3_OK)
    {
        fclose(fp);
        return status;
    }

    long audio_offset = audio_start_offset(&hdr);

    int target_index = find_field_frame(frames, frame_count, target_id, field,
                                        hdr.major_version);

    if (target_index < 0)
    {
        free_all_frames(frames, frame_count);
        fclose(fp);
        return ID3_ERR_NOT_FOUND;
    }

    /* Remove it: free its data, then shift every frame after it back
     * one slot. This keeps the array contiguous with no gaps. */
    free_frame(&frames[target_index]);
    for (int i = target_index; i < frame_count - 1; i++)
    {
        frames[i] = frames[i + 1];
    }
    frame_count--;

    char temp_filename[1024];
    snprintf(temp_filename, sizeof(temp_filename), "%s.tmp", filename);

    FILE *out = fopen(temp_filename, "wb");
    if (out == NULL)
    {
        free_all_frames(frames, frame_count);
        fclose(fp);
        return ID3_ERR_OPEN;
    }

    status = write_tag(out, hdr.major_version, frames, frame_count);
    if (status == ID3_OK && fseek(fp, audio_offset, SEEK_SET) == 0)
    {
        status = copy_remaining_bytes(fp, out);
    }
    else if (status == ID3_OK)
    {
        status = ID3_ERR_READ;
    }

    free_all_frames(frames, frame_count);
    fclose(fp);
    fclose(out);

    if (status != ID3_OK)
    {
        remove(temp_filename);
        return status;
    }

    if (replace_file(temp_filename, filename) != 0)
    {
        return ID3_ERR_WRITE;
    }

    return ID3_OK;
}

/*
 * delete_all_tags() -- additional feature: strip the whole ID3 tag.
 *
 * Simplest of the three writers: no frames to think about at all, just
 * copy the audio data (everything after the original tag) into a new
 * file that has no ID3 header in front of it.
 */
Status delete_all_tags(const char *filename)
{
    if (filename == NULL)
    {
        return ID3_ERR_ARGS;
    }

    FILE *fp = fopen(filename, "rb");
    if (fp == NULL)
    {
        return ID3_ERR_OPEN;
    }

    Header hdr;
    Status status = read_header(fp, &hdr);
    if (status != ID3_OK)
    {
        fclose(fp); /* includes ID3_ERR_NO_TAG: nothing to delete */
        return status;
    }

    long audio_offset = audio_start_offset(&hdr);

    char temp_filename[1024];
    snprintf(temp_filename, sizeof(temp_filename), "%s.tmp", filename);

    FILE *out = fopen(temp_filename, "wb");
    if (out == NULL)
    {
        fclose(fp);
        return ID3_ERR_OPEN;
    }

    if (fseek(fp, audio_offset, SEEK_SET) != 0)
    {
        fclose(fp);
        fclose(out);
        remove(temp_filename);
        return ID3_ERR_READ;
    }

    status = copy_remaining_bytes(fp, out);

    fclose(fp);
    fclose(out);

    if (status != ID3_OK)
    {
        remove(temp_filename);
        return status;
    }

    if (replace_file(temp_filename, filename) != 0)
    {
        return ID3_ERR_WRITE;
    }

    return ID3_OK;
}

const char *status_to_string(Status s)
{
    switch (s)
    {
        case ID3_OK:
            return "OK";
        case ID3_ERR_ARGS:
            return "ERROR: Incorrect format of command line arguments.\nUse \"./mp3_tag_reader -h\" for help";
        case ID3_ERR_NOT_MP3:
            return "ERROR: File does not have a .mp3 extension.";
        case ID3_ERR_OPEN:
            return "ERROR: Could not open the specified file.";
        case ID3_ERR_READ:
            return "ERROR: Failed while reading the file (it may be truncated or corrupt).";
        case ID3_ERR_WRITE:
            return "ERROR: Failed while writing the updated file.";
        case ID3_ERR_NO_TAG:
            return "ERROR: No ID3 tag found in this file.";
        case ID3_ERR_UNSUPPORTED:
            return "ERROR: Unsupported ID3 version (only ID3v2.2, v2.3 and v2.4 are supported).";
        case ID3_ERR_NOMEM:
            return "ERROR: Out of memory.";
        case ID3_ERR_NOT_FOUND:
            return "ERROR: Requested tag was not found in this file.";
        case ID3_ERR_BAD_MODIFIER:
            return "ERROR: Unknown modifier. Use \"./mp3_tag_reader -h\" for help";
        case ID3_END_OF_FRAMES:
            return "End of frames.";
        default:
            return "ERROR: Unknown error.";
    }
}