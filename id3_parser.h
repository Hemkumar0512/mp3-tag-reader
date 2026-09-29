/*
 * id3_parser.h
 * ------------
 * Public interface for reading and editing ID3v2.2, ID3v2.3 and ID3v2.4
 * tags in MP3 files (v2.4 support covers the common case: header parsing
 * and syncsafe frame sizes; the rarer v2.4-only features -- per-frame
 * unsynchronisation, the data-length indicator, and the optional footer
 * -- are not implemented).
 *
 * This header is the "contract" between id3_parser.c (which knows how
 * ID3 tags are laid out on disk) and main.c (which only knows about
 * command-line arguments and printing). main.c never touches a FILE *
 * for tag data directly -- it just calls parse_tag() / modify_tag_field()
 * and gets back a filled-in TagInfo struct or a Status code.
 */

#ifndef ID3_PARSER_H
#define ID3_PARSER_H

#include <stdio.h>   /* needed because several signatures below use FILE * */

/* ------------------------------------------------------------------ */
/* Status codes                                                        */
/* ------------------------------------------------------------------ */

/*
 * Every function that can fail returns one of these instead of using
 * printf() internally. This keeps the parser reusable: main.c decides
 * *what* to print (and in what wording) for each failure, the parser
 * only decides *that* something failed and why.
 */
typedef enum status
{
    ID3_OK = 0,           /* success                                       */
    ID3_ERR_ARGS,         /* bad function arguments (e.g. NULL pointer)    */
    ID3_ERR_NOT_MP3,      /* filename does not end in ".mp3"               */
    ID3_ERR_OPEN,         /* fopen() failed                                */
    ID3_ERR_READ,         /* short read / I/O error while reading          */
    ID3_ERR_WRITE,        /* short write / I/O error while writing         */
    ID3_ERR_NO_TAG,       /* file does not start with "ID3"                */
    ID3_ERR_UNSUPPORTED,  /* ID3 version we do not handle (not 2.2/2.3/2.4) */
    ID3_ERR_NOMEM,        /* malloc() failed                               */
    ID3_ERR_NOT_FOUND,    /* requested frame not present in the tag        */
    ID3_ERR_BAD_MODIFIER, /* unknown -t/-T/-a/-A/-y/-c/-g style flag       */
    ID3_END_OF_FRAMES     /* not an error: hit padding / end of frame list */
} Status;

/* ------------------------------------------------------------------ */
/* On-disk structures, decoded into memory                             */
/* ------------------------------------------------------------------ */

/*
 * The 10-byte ID3v2 tag header. This layout is IDENTICAL across v2.2,
 * v2.3 and v2.4 -- only major_version differs, and only the FRAMES
 * that follow are laid out differently per version (see Frame below).
 *
 * NOTE: this struct is intentionally *not* read with one fread() call.
 * On disk, "identifier" is 3 bytes (we store 4, so we can NUL-terminate
 * it) and "size" is 4 syncsafe bytes (we store it already decoded into
 * a normal integer). Because the in-memory layout does not match the
 * on-disk layout byte-for-byte, id3_parser.c reads each field
 * separately instead of doing a single fread(&hdr, ...).
 */
typedef struct header
{
    char          identifier[4];   /* "ID3" + '\0'                        */
    unsigned char major_version;   /* 2, 3, or 4                          */
    unsigned char revision;        /* usually 0, not used here            */
    unsigned char flags;           /* bit 6 = extended header present     */
    unsigned int  size;            /* DECODED syncsafe size of all frames,
                                       i.e. total tag size minus these
                                       10 header bytes                    */
} Header;

/*
 * One decoded frame, version-independent once in memory.
 *
 * On disk the frame header layout actually differs by version:
 *   v2.2: 3-byte ID, 3-byte plain big-endian size, NO flags   (6 bytes)
 *   v2.3: 4-byte ID, 4-byte plain big-endian size, 2-byte flags (10 bytes)
 *   v2.4: 4-byte ID, 4-byte SYNCSAFE size,          2-byte flags (10 bytes)
 * read_frame() hides these differences: whichever version it reads, it
 * hands back the same Frame shape with "size" already decoded. "flags"
 * is unused (left as zero) for v2.2, since that version has none.
 */
typedef struct frame
{
    char           frame_id[5];  /* e.g. "TIT2" (v2.3/2.4) or "TT2" (v2.2)
                                     + '\0'                               */
    unsigned int   size;         /* DECODED size of actual_data, in bytes */
    unsigned char  flags[2];     /* frame flags, copied through unchanged;
                                     always {0,0} for a v2.2 frame        */
    unsigned char *actual_data;  /* heap-allocated, exactly "size" bytes  */
} Frame;

/* ------------------------------------------------------------------ */
/* The struct main.c actually works with                               */
/* ------------------------------------------------------------------ */

/*
 * Buffer sizes for the parsed text fields. Centralising them as macros
 * means every bounded copy (strncpy/snprintf) and the struct itself
 * always agree on the limit.
 */
#define MAX_TEXT    128  /* title, artist, album, genre                  */
#define MAX_COMMENT 256
#define MAX_TRACK    16  /* e.g. "7/8"                                   */
#define MAX_YEAR      5  /* 4 digits + '\0'                              */
#define MAX_MIME     32  /* e.g. "image/jpeg"                            */

/*
 * The fully parsed tag, ready to print or hand to the editor.
 *
 * Convention used throughout this project: an EMPTY STRING means "this
 * field was not present in the file". For that convention to hold, the
 * whole struct must be zeroed (memset to 0) before parsing begins --
 * parse_tag() takes care of that.
 */
typedef struct tag_info
{
    Header header;               /* keeps version + total tag size handy */

    char title[MAX_TEXT];        /* TIT2 */
    char artist[MAX_TEXT];       /* TPE1 */
    char album[MAX_TEXT];        /* TALB */
    char year[MAX_YEAR];         /* TYER */
    char track[MAX_TRACK];       /* TRCK, kept as text ("7/8"), not int   */
    char genre[MAX_TEXT];        /* TCON */
    char comment[MAX_COMMENT];   /* COMM, text portion only               */

    /* Embedded album art (APIC frame), if any */
    int            has_art;        /* 0 = not found, 1 = found            */
    char           art_mime[MAX_MIME];
    unsigned char  art_type;       /* ID3 "picture type" byte             */
    unsigned int   art_size;       /* size of the IMAGE bytes only        */
    unsigned char *art_data;       /* heap-allocated copy of image bytes,
                                       NULL if has_art == 0. Caller must
                                       free() this exactly once.          */
} TagInfo;

/* ------------------------------------------------------------------ */
/* Function declarations                                               */
/* ------------------------------------------------------------------ */

/* Argument validation -------------------------------------------------*/

/* Returns ID3_OK if filename ends in ".mp3" (case-sensitive), else
 * ID3_ERR_NOT_MP3. */
Status check_mp3_extension(const char *filename);

/* Low-level readers (each seeks to the position it needs itself, so
 * callers never have to track file position manually) ----------------*/

Status read_header(FILE *fp, Header *hdr);

/* major_version must be the value read into hdr->major_version by
 * read_header() -- it decides whether this frame is read in v2.2's
 * 6-byte-header form or v2.3/v2.4's 10-byte-header form. */
Status read_frame(FILE *fp, unsigned char major_version, Frame *frame);
void   free_frame(Frame *frame);

/* Top-level read API used by main.c -----------------------------------*/

Status parse_tag(const char *filename, TagInfo *info);
void   print_tag(const TagInfo *info);
void   free_tag_info(TagInfo *info); /* frees info->art_data if set */

/* Editing API ----------------------------------------------------------
 * 'field' is one of: 't' 'T' 'a' 'A' 'y' 'c' 'g' (matching the -t/-T/...
 * command-line modifiers). new_value is the replacement text.
 *
 * All three functions below preserve whatever ID3 major_version the
 * file already used (2, 3, or 4) -- editing never silently upgrades or
 * downgrades the tag version.
 */
Status modify_tag_field(const char *filename, char field, const char *new_value);

/* Additional feature: removes ONE frame matching 'field' from the tag,
 * rewriting the file without it. Returns ID3_ERR_NOT_FOUND if that
 * field wasn't present to begin with. */
Status delete_tag_field(const char *filename, char field);

/* Additional feature: strips the ENTIRE ID3 tag from the file, leaving
 * only the audio data. Safe to call on a file that turns out to have
 * no tag -- it returns whatever read_header() reports instead of
 * touching the file. */
Status delete_all_tags(const char *filename);

/* Returns a human-readable field name for a modifier character, e.g.
 * field_name('t') -> "Title". Used for the "<Field> Modification - Done"
 * message. Returns NULL if the character is not a recognised modifier. */
const char *field_name(char field);

/* Turns a Status into the message main.c should print. */
const char *status_to_string(Status s);

#endif /* ID3_PARSER_H */