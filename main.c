/*
 * main.c
 * ------
 * Command-line front end for the MP3 Tag Reader & Editor.
 *
 * This file deliberately knows NOTHING about ID3 byte layouts -- all of
 * that lives in id3_parser.c. Its only jobs are:
 *   1. Validate argc/argv.
 *   2. Call the right id3_parser function.
 *   3. Print the right message for whatever Status comes back.
 *
 * Usage (the first block matches the spec's sample output exactly; -D
 * and -r are additional features beyond the spec's mandatory set):
 *   ./mp3_tag_reader -h
 *   ./mp3_tag_reader -v <filename.mp3>
 *   ./mp3_tag_reader -e <filename.mp3> <modifier> "<new value>"
 *   ./mp3_tag_reader -D <filename.mp3>              (delete ALL tag data)
 *   ./mp3_tag_reader -r <filename.mp3> <modifier>   (delete ONE tag)
 */

#include <stdio.h>
#include <string.h>
#include "id3_parser.h"

static void print_usage_error(void)
{
    printf("ERROR: Incorrect format of command line arguments.\n");
    printf("Use \"./mp3_tag_reader -h\" for help\n");
}

static void print_help(void)
{
    /* This block matches the spec's Fig 5.2 exactly. */
    printf("Help menu for Mp3 Tag Reader and Editor:\n\n");
    printf("For viewing the tags-    ./mp3_tag_reader -v <filename.mp3>\n\n");
    printf("For editing the tags-    ./mp3_tag_reader -e <filename.mp3> <modifier>\n\n");
    printf("Modifier Function\n");
    printf("-t          Modify Title Tag\n");
    printf("-T          Modify Track Tag\n");
    printf("-a          Modify Artist Tag\n");
    printf("-A          Modify Album Tag\n");
    printf("-y          Modify Year Tag\n");
    printf("-c          Modify Comment Tag\n");
    printf("-g          Modify Genre Tag\n");

    /* Additional features, beyond the spec's required sample output. */
    printf("\nAdditional options:\n");
    printf("For deleting all tags-   ./mp3_tag_reader -D <filename.mp3>\n");
    printf("For deleting one tag-    ./mp3_tag_reader -r <filename.mp3> <modifier>\n");
    printf("(same modifiers as above apply to -r)\n");
}

/* Chooses a file extension for extracted album art based on the MIME
 * type (v2.3/v2.4) or format code (v2.2) recorded in the tag. */
static const char *art_extension(const char *mime)
{
    if (strstr(mime, "jpeg") != NULL || strstr(mime, "jpg") != NULL ||
        strstr(mime, "JPG")  != NULL || strstr(mime, "JPEG") != NULL)
    {
        return ".jpg";
    }
    if (strstr(mime, "png") != NULL || strstr(mime, "PNG") != NULL)
    {
        return ".png";
    }
    return ".img"; /* unknown format: fall back to a generic extension */
}

/* Handles: ./mp3_tag_reader -v <filename.mp3> */
static int handle_view(const char *filename)
{
    Status status = check_mp3_extension(filename);
    if (status != ID3_OK)
    {
        printf("%s\n", status_to_string(status));
        return 1;
    }

    TagInfo info;
    status = parse_tag(filename, &info);
    if (status != ID3_OK)
    {
        printf("%s\n", status_to_string(status));
        return 1;
    }

    print_tag(&info);

    /* Mandatory requirement: "If any image embedded in file, display
     * details about image (Type, path and size)." We report the type
     * and size here, then extract the bytes to a file and report that
     * path too -- rather than just announcing that art exists. */
    if (info.has_art && info.art_data != NULL)
    {
        const char *ext = art_extension(info.art_mime);
        char art_filename[64];
        snprintf(art_filename, sizeof(art_filename), "album_art%s", ext);

        FILE *art_fp = fopen(art_filename, "wb");
        if (art_fp != NULL)
        {
            fwrite(info.art_data, 1, info.art_size, art_fp);
            fclose(art_fp);

            printf("\nAlbum Art:\n");
            printf("  Type : %s\n", info.art_mime);
            printf("  Size : %u bytes\n", info.art_size);
            printf("  Path : %s\n", art_filename);
            printf("\nExtracting Album Art - Done\n");
        }
        /* If fopen() failed we simply skip the extraction; the rest of
         * the view output has already printed correctly. */
    }

    free_tag_info(&info);
    return 0;
}

/* Handles: ./mp3_tag_reader -e <filename.mp3> <modifier> <new value> */
static int handle_edit(const char *filename, const char *modifier, const char *new_value)
{
    Status status = check_mp3_extension(filename);
    if (status != ID3_OK)
    {
        printf("%s\n", status_to_string(status));
        return 1;
    }

    if (strlen(modifier) != 2 || modifier[0] != '-')
    {
        printf("%s\n", status_to_string(ID3_ERR_BAD_MODIFIER));
        return 1;
    }
    char field = modifier[1];

    const char *name = field_name(field);
    if (name == NULL)
    {
        printf("%s\n", status_to_string(ID3_ERR_BAD_MODIFIER));
        return 1;
    }

    status = modify_tag_field(filename, field, new_value);
    if (status != ID3_OK)
    {
        printf("%s\n", status_to_string(status));
        return 1;
    }

    /* Re-read the file so we print the tag as it now stands on disk --
     * this also doubles as a sanity check that our own write is valid
     * and readable. */
    TagInfo info;
    status = parse_tag(filename, &info);
    if (status != ID3_OK)
    {
        printf("%s\n", status_to_string(status));
        return 1;
    }

    print_tag(&info);
    printf("\n%s Modification - Done\n", name);

    free_tag_info(&info);
    return 0;
}

/* Handles: ./mp3_tag_reader -D <filename.mp3>   (additional feature) */
static int handle_delete_all(const char *filename)
{
    Status status = check_mp3_extension(filename);
    if (status != ID3_OK)
    {
        printf("%s\n", status_to_string(status));
        return 1;
    }

    status = delete_all_tags(filename);
    if (status != ID3_OK)
    {
        printf("%s\n", status_to_string(status));
        return 1;
    }

    printf("All ID3 tag data has been removed from %s\n", filename);
    return 0;
}

/* Handles: ./mp3_tag_reader -r <filename.mp3> <modifier>  (additional feature) */
static int handle_delete_one(const char *filename, const char *modifier)
{
    Status status = check_mp3_extension(filename);
    if (status != ID3_OK)
    {
        printf("%s\n", status_to_string(status));
        return 1;
    }

    if (strlen(modifier) != 2 || modifier[0] != '-')
    {
        printf("%s\n", status_to_string(ID3_ERR_BAD_MODIFIER));
        return 1;
    }
    char field = modifier[1];

    const char *name = field_name(field);
    if (name == NULL)
    {
        printf("%s\n", status_to_string(ID3_ERR_BAD_MODIFIER));
        return 1;
    }

    status = delete_tag_field(filename, field);
    if (status != ID3_OK)
    {
        printf("%s\n", status_to_string(status));
        return 1;
    }

    printf("%s Tag Deleted\n", name);
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        print_usage_error();
        return 1;
    }

    if (strcmp(argv[1], "-h") == 0)
    {
        print_help();
        return 0;
    }

    if (strcmp(argv[1], "-v") == 0)
    {
        if (argc != 3)
        {
            print_usage_error();
            return 1;
        }
        return handle_view(argv[2]);
    }

    if (strcmp(argv[1], "-e") == 0)
    {
        if (argc != 5)
        {
            print_usage_error();
            return 1;
        }
        return handle_edit(argv[2], argv[3], argv[4]);
    }

    if (strcmp(argv[1], "-D") == 0)
    {
        if (argc != 3)
        {
            print_usage_error();
            return 1;
        }
        return handle_delete_all(argv[2]);
    }

    if (strcmp(argv[1], "-r") == 0)
    {
        if (argc != 4)
        {
            print_usage_error();
            return 1;
        }
        return handle_delete_one(argv[2], argv[3]);
    }

    print_usage_error();
    return 1;
}