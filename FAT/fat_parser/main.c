#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t cluster_size;
    uint32_t root_cluster;
    uint64_t fat_offset;
    uint64_t fat_size_bytes;
    uint64_t data_offset;
    uint32_t fat_entries;
} Fat32;

static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0]
           | ((uint32_t)p[1] << 8)
           | ((uint32_t)p[2] << 16)
           | ((uint32_t)p[3] << 24);
}

static int read_at(FILE *fp, uint64_t offset, void *buffer, size_t size)
{
    if (offset > (uint64_t)LONG_MAX) {
        return -1;
    }
    if (fseek(fp, (long)offset, SEEK_SET) != 0) {
        return -1;
    }
    return fread(buffer, 1, size, fp) == size ? 0 : -1;
}

static int parse_bpb(FILE *fp, Fat32 *fs)
{
    uint8_t boot[512];
    uint16_t reserved_sectors;
    uint16_t root_entry_count;
    uint16_t fat_size_16;
    uint32_t fat_size_32;
    uint8_t fat_count;

    if (read_at(fp, 0, boot, sizeof(boot)) != 0) {
        fprintf(stderr, "cannot read the FAT32 boot sector\n");
        return -1;
    }

    fs->bytes_per_sector = get_le16(boot + 11);
    fs->sectors_per_cluster = boot[13];
    reserved_sectors = get_le16(boot + 14);
    fat_count = boot[16];
    root_entry_count = get_le16(boot + 17);
    fat_size_16 = get_le16(boot + 22);
    fat_size_32 = get_le32(boot + 36);
    fs->root_cluster = get_le32(boot + 44);

    if (!(fs->bytes_per_sector == 512 || fs->bytes_per_sector == 1024 ||
          fs->bytes_per_sector == 2048 || fs->bytes_per_sector == 4096) ||
        fs->sectors_per_cluster == 0 ||
        (fs->sectors_per_cluster & (fs->sectors_per_cluster - 1)) != 0 ||
        reserved_sectors == 0 || fat_count == 0 || fat_size_32 == 0 ||
        root_entry_count != 0 || fat_size_16 != 0 || fs->root_cluster < 2) {
        fprintf(stderr, "invalid or unsupported FAT32 image\n");
        return -1;
    }

    fs->cluster_size = fs->bytes_per_sector * fs->sectors_per_cluster;
    fs->fat_offset = (uint64_t)reserved_sectors * fs->bytes_per_sector;
    fs->fat_size_bytes = (uint64_t)fat_size_32 * fs->bytes_per_sector;
    fs->data_offset = ((uint64_t)reserved_sectors +
                       (uint64_t)fat_count * fat_size_32) *
                      fs->bytes_per_sector;
    fs->fat_entries = (uint32_t)(fs->fat_size_bytes / 4);

    if (fs->fat_entries <= fs->root_cluster) {
        fprintf(stderr, "root cluster is outside the FAT\n");
        return -1;
    }
    return 0;
}

static int read_fat_entry(FILE *fp, const Fat32 *fs, uint32_t cluster,
                          uint32_t *next)
{
    uint8_t raw[4];

    if (cluster >= fs->fat_entries ||
        read_at(fp, fs->fat_offset + (uint64_t)cluster * 4, raw, sizeof(raw)) != 0) {
        return -1;
    }
    *next = get_le32(raw) & 0x0FFFFFFFU;
    return 0;
}

static void short_name(const uint8_t *entry, char name[13])
{
    size_t base_len = 8;
    size_t ext_len = 3;
    size_t pos = 0;
    size_t i;
    uint8_t nt_flags = entry[12];

    while (base_len > 0 && entry[base_len - 1] == ' ') {
        --base_len;
    }
    while (ext_len > 0 && entry[8 + ext_len - 1] == ' ') {
        --ext_len;
    }

    for (i = 0; i < base_len; ++i) {
        unsigned char ch = entry[i];
        if (i == 0 && ch == 0x05) {
            ch = 0xE5;
        }
        if ((nt_flags & 0x08) && ch < 0x80) {
            ch = (unsigned char)tolower(ch);
        }
        name[pos++] = (char)ch;
    }

    if (ext_len > 0) {
        name[pos++] = '.';
        for (i = 0; i < ext_len; ++i) {
            unsigned char ch = entry[8 + i];
            if ((nt_flags & 0x10) && ch < 0x80) {
                ch = (unsigned char)tolower(ch);
            }
            name[pos++] = (char)ch;
        }
    }
    name[pos] = '\0';
}

static int print_entry(const uint8_t *entry)
{
    uint8_t attributes = entry[11];
    uint32_t start_cluster;
    uint32_t file_size;
    char name[13];

    if (entry[0] == 0xE5 || attributes == 0x0F || (attributes & 0x08)) {
        return 0;
    }

    short_name(entry, name);
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return 0;
    }

    start_cluster = ((uint32_t)get_le16(entry + 20) << 16) |
                    get_le16(entry + 26);
    file_size = get_le32(entry + 28);
    printf("%s,%" PRIu32 ",%" PRIu32 "\n", name, start_cluster, file_size);
    return ferror(stdout) ? -1 : 0;
}

static int print_root_directory(FILE *fp, const Fat32 *fs)
{
    uint8_t *cluster_data = NULL;
    uint8_t *visited = NULL;
    size_t visited_size = ((size_t)fs->fat_entries + 7) / 8;
    uint32_t cluster = fs->root_cluster;
    int result = -1;

    cluster_data = malloc(fs->cluster_size);
    visited = calloc(visited_size, 1);
    if (cluster_data == NULL || visited == NULL) {
        fprintf(stderr, "out of memory\n");
        goto cleanup;
    }

    for (;;) {
        uint64_t offset;
        uint32_t next;
        size_t i;

        if (cluster < 2 || cluster >= fs->fat_entries) {
            fprintf(stderr, "invalid cluster in root directory chain\n");
            goto cleanup;
        }
        if (visited[cluster / 8] & (uint8_t)(1U << (cluster % 8))) {
            fprintf(stderr, "cycle in root directory cluster chain\n");
            goto cleanup;
        }
        visited[cluster / 8] |= (uint8_t)(1U << (cluster % 8));

        offset = fs->data_offset + (uint64_t)(cluster - 2) * fs->cluster_size;
        if (read_at(fp, offset, cluster_data, fs->cluster_size) != 0) {
            fprintf(stderr, "cannot read root directory cluster\n");
            goto cleanup;
        }

        for (i = 0; i < fs->cluster_size; i += 32) {
            if (cluster_data[i] == 0x00) {
                result = 0;
                goto cleanup;
            }
            if (print_entry(cluster_data + i) != 0) {
                fprintf(stderr, "cannot write output\n");
                goto cleanup;
            }
        }

        if (read_fat_entry(fp, fs, cluster, &next) != 0) {
            fprintf(stderr, "cannot read FAT entry\n");
            goto cleanup;
        }
        if (next >= 0x0FFFFFF8U) {
            result = 0;
            goto cleanup;
        }
        if (next == 0x0FFFFFF7U || next < 2) {
            fprintf(stderr, "broken root directory cluster chain\n");
            goto cleanup;
        }
        cluster = next;
    }

cleanup:
    free(visited);
    free(cluster_data);
    return result;
}

int main(int argc, char **argv)
{
    FILE *fp;
    Fat32 fs;
    int result;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <FAT32 image>\n", argv[0]);
        return EXIT_FAILURE;
    }

    fp = fopen(argv[1], "rb");
    if (fp == NULL) {
        fprintf(stderr, "%s: %s\n", argv[1], strerror(errno));
        return EXIT_FAILURE;
    }

    result = parse_bpb(fp, &fs);
    if (result == 0) {
        result = print_root_directory(fp, &fs);
    }
    fclose(fp);
    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
