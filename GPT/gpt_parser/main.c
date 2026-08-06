#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <sys/types.h>

#define GPT_HEADER_MIN_SIZE 92
#define GPT_ENTRY_MIN_SIZE  128

static void usage(const char *program_name)
{
    fprintf(stderr, "Usage: %s <evidence image>\n", program_name);
}

static uint32_t read_little_endian32(const uint8_t *data)
{
    return ((uint32_t)data[0])       |
           ((uint32_t)data[1] << 8)  |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static uint64_t read_little_endian64(const uint8_t *data)
{
    return ((uint64_t)data[0])       |
           ((uint64_t)data[1] << 8)  |
           ((uint64_t)data[2] << 16) |
           ((uint64_t)data[3] << 24) |
           ((uint64_t)data[4] << 32) |
           ((uint64_t)data[5] << 40) |
           ((uint64_t)data[6] << 48) |
           ((uint64_t)data[7] << 56);
}

static int read_at(FILE *image,
                   uint64_t byte_offset,
                   void *buffer,
                   size_t size)
{
    if (byte_offset > (uint64_t)INT64_MAX) {
        return 0;
    }

    if (fseeko(image, (off_t)byte_offset, SEEK_SET) != 0) {
        return 0;
    }

    if (fread(buffer, 1, size, image) != size) {
        return 0;
    }

    return 1;
}

/*
 * GPT 헤더는 LBA 1에 있다.
 * 512바이트와 4096바이트 논리 섹터를 검사한다.
 */
static uint32_t detect_sector_size(FILE *image)
{
    const uint32_t candidates[] = {512, 4096};
    uint8_t signature[8];

    for (size_t i = 0;
         i < sizeof(candidates) / sizeof(candidates[0]);
         i++) {
        uint32_t sector_size = candidates[i];

        if (read_at(image,
                    sector_size,
                    signature,
                    sizeof(signature)) &&
            memcmp(signature, "EFI PART", 8) == 0) {
            return sector_size;
        }
    }

    return 0;
}

static int is_zero_guid(const uint8_t *guid)
{
    for (int i = 0; i < 16; i++) {
        if (guid[i] != 0) {
            return 0;
        }
    }

    return 1;
}

/*
 * GPT에 저장된 GUID를 과제 예시의 표준 표시 순서로 바꾼다.
 *
 * 디스크 바이트:
 * A2 A0 D0 EB E5 B9 33 44 87 C0 68 B6 B7 26 99 C7
 *
 * 출력:
 * EBD0A0A2B9E5443387C068B6B72699C7
 */
static void format_guid(const uint8_t *guid, char output[33])
{
    snprintf(
        output,
        33,
        "%02X%02X%02X%02X"
        "%02X%02X"
        "%02X%02X"
        "%02X%02X"
        "%02X%02X%02X%02X%02X%02X",

        guid[3], guid[2], guid[1], guid[0],
        guid[5], guid[4],
        guid[7], guid[6],
        guid[8], guid[9],
        guid[10], guid[11], guid[12],
        guid[13], guid[14], guid[15]
        );
}

static const char *detect_filesystem(FILE *image,
                                     uint64_t start_lba,
                                     uint32_t sector_size)
{
    uint8_t boot_sector[512];

    if (start_lba > UINT64_MAX / sector_size) {
        return "UNKNOWN";
    }

    uint64_t byte_offset = start_lba * sector_size;

    if (!read_at(image,
                 byte_offset,
                 boot_sector,
                 sizeof(boot_sector))) {
        return "UNKNOWN";
    }

    /*
     * NTFS 부트 섹터의 OEM ID:
     * 오프셋 3부터 "NTFS    "
     */
    if (memcmp(boot_sector + 3, "NTFS    ", 8) == 0) {
        return "NTFS";
    }

    /*
     * FAT32 부트 섹터의 File System Type:
     * 오프셋 82부터 "FAT32   "
     */
    if (memcmp(boot_sector + 82, "FAT32   ", 8) == 0) {
        return "FAT32";
    }

    return "UNKNOWN";
}

static int parse_gpt(FILE *image, uint32_t sector_size)
{
    uint8_t header[GPT_HEADER_MIN_SIZE];

    /*
     * GPT 헤더는 LBA 1에 있다.
     *
     * byte offset = 1 * sector_size
     */
    if (!read_at(image,
                 sector_size,
                 header,
                 sizeof(header))) {
        fprintf(stderr, "Failed to read GPT header\n");
        return 0;
    }

    if (memcmp(header, "EFI PART", 8) != 0) {
        fprintf(stderr, "Invalid GPT signature\n");
        return 0;
    }

    uint32_t header_size =
        read_little_endian32(header + 12);

    uint64_t entry_array_lba =
        read_little_endian64(header + 72);

    uint32_t entry_count =
        read_little_endian32(header + 80);

    uint32_t entry_size =
        read_little_endian32(header + 84);

    if (header_size < GPT_HEADER_MIN_SIZE ||
        header_size > sector_size) {
        fprintf(stderr, "Invalid GPT header size\n");
        return 0;
    }

    if (entry_count == 0 ||
        entry_size < GPT_ENTRY_MIN_SIZE) {
        fprintf(stderr, "Invalid GPT entry information\n");
        return 0;
    }

    if (entry_array_lba > UINT64_MAX / sector_size) {
        fprintf(stderr, "Invalid GPT entry array location\n");
        return 0;
    }

    uint64_t entry_array_offset =
        entry_array_lba * sector_size;

    for (uint32_t i = 0; i < entry_count; i++) {
        uint8_t entry[48];

        if ((uint64_t)i >
            (UINT64_MAX - entry_array_offset) / entry_size) {
            fprintf(stderr, "GPT entry offset overflow\n");
            return 0;
        }

        uint64_t entry_offset =
            entry_array_offset + (uint64_t)i * entry_size;

        /*
         * Type GUID부터 Ending LBA까지는 처음 48바이트다.
         */
        if (!read_at(image,
                     entry_offset,
                     entry,
                     sizeof(entry))) {
            fprintf(stderr, "Failed to read GPT entry\n");
            return 0;
        }

        /*
         * Partition Type GUID가 모두 0이면
         * 사용하지 않는 엔트리다.
         */
        if (is_zero_guid(entry)) {
            continue;
        }

        /*
         * GPT 엔트리 구조:
         *
         * 0~15  : Partition Type GUID
         * 16~31 : Unique Partition GUID
         * 32~39 : Starting LBA
         * 40~47 : Ending LBA
         */
        uint64_t start_lba =
            read_little_endian64(entry + 32);

        uint64_t end_lba =
            read_little_endian64(entry + 40);

        if (end_lba < start_lba ||
            end_lba == UINT64_MAX) {
            fprintf(stderr, "Invalid GPT partition range\n");
            return 0;
        }

        /*
         * 시작과 마지막 LBA를 모두 포함하므로 +1
         */
        uint64_t partition_size =
            end_lba - start_lba + 1;

        char guid_string[33];

        /*
         * 과제 예시는 Partition Type GUID를 출력한다.
         */
        format_guid(entry, guid_string);

        const char *filesystem =
            detect_filesystem(image,
                              start_lba,
                              sector_size);

        /*
         * 정상 결과는 stdout에만 출력한다.
         * 각 항목은 공백 한 칸으로 구분한다.
         */
        printf("%s %s %" PRIu64 " %" PRIu64 "\n",
               guid_string,
               filesystem,
               start_lba,
               partition_size);
    }

    return 1;
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        usage(argv[0]);
        return 1;
    }

    FILE *image = fopen(argv[1], "rb");

    if (image == NULL) {
        fprintf(stderr,
                "Failed to open file: %s\n",
                argv[1]);
        return 1;
    }

    uint32_t sector_size =
        detect_sector_size(image);

    if (sector_size == 0) {
        fprintf(stderr, "GPT header not found\n");
        fclose(image);
        return 1;
    }

    if (!parse_gpt(image, sector_size)) {
        fclose(image);
        return 1;
    }

    fclose(image);
    return 0;
}