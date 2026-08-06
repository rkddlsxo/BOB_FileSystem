#include <stdio.h>
#include <stdint.h>
#include <sys/types.h>

#define SECTOR_SIZE 512
#define PARTITION_TABLE_OFFSET 446
#define PARTITION_ENTRY_SIZE 16
#define MAX_EBR_COUNT 10024

void usage(){
    fprintf(stdout, "Usage: <evidence image>\n");
    return;
}

uint32_t read_little_endian32(const uint8_t* data){
    uint32_t d = ((uint32_t)data[0]) | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
    return d;
}

const char* get_filesystem_type(uint8_t type){
    if (type == 0x0B || type == 0x0C || type == 0x1B || type == 0x1C) {
        return "FAT32";
    }

    if (type == 0x07) {
        return "NTFS";
    }

    return "UNKNOWN";
}

int is_extended(uint8_t type){
    if(type == 0x05 || type == 0x0F || type == 0x85){
        return 1;
    }

    return 0;
}

int read_sector(FILE* image, uint32_t lba, uint8_t* sector){
    off_t byte_offset = (off_t)lba * SECTOR_SIZE;


    if(fseeko(image, byte_offset, SEEK_SET) != 0){
        return 0;
    }

    if (fread(sector, 1, SECTOR_SIZE, image) != SECTOR_SIZE) {
        return 0;
    }

    return 1;
}

int parse_ebr(FILE* image, uint32_t extended_base){
    uint8_t ebr_sector[SECTOR_SIZE];

    uint32_t current_ebr = extended_base;

    for(int count = 0; count < MAX_EBR_COUNT; count++){
        if(!read_sector(image, current_ebr, ebr_sector)){
            fprintf(stderr, "FAIL");
            return 0;
        }

        if(ebr_sector[510] != 0x55 || ebr_sector[511] != 0xAA){
            return 0;
        }

        const uint8_t* logical_entry = &ebr_sector[PARTITION_TABLE_OFFSET];
        uint8_t logical_type = logical_entry[4];
        uint32_t logical_relative_start = read_little_endian32(logical_entry + 8);
        uint32_t logical_size = read_little_endian32(logical_entry + 12);

        if(logical_type != 0x00 && logical_size != 0){
            uint64_t logical_abs_start = (uint64_t)current_ebr + logical_relative_start;

            printf("%s,%llu,%u\n",
                   get_filesystem_type(logical_type),
                   (unsigned long long)logical_abs_start,
                   (unsigned int)logical_size);
        }


        const uint8_t *next_entry =
            &ebr_sector[PARTITION_TABLE_OFFSET +
                        PARTITION_ENTRY_SIZE];

        uint8_t next_type = next_entry[4];

        uint32_t next_relative_start =
            read_little_endian32(next_entry + 8);

        if (next_type == 0x00 || next_relative_start == 0) {
            return 1;
        }

        if (!is_extended(next_type)) {
            fprintf(stderr, "Invalid EBR link type\n");
            return 0;
        }


        uint64_t next_ebr =
            (uint64_t)extended_base + next_relative_start;

        if (next_ebr > UINT32_MAX ||
            next_ebr == current_ebr) {
            fprintf(stderr, "Invalid EBR link\n");
            return 0;
        }

        current_ebr = (uint32_t)next_ebr;
    }

    return 0;
}

int main(int argc, char* argv[])
{
    FILE* image;
    uint8_t sector[SECTOR_SIZE];

    if(argc != 2){
        usage();
        return 1;
    }

    image = fopen(argv[1], "rb"); // open evidence image binary read
    if(image == NULL){ // failed to open file
        return 1;
    }

    if(!read_sector(image, 0, sector)){ //  non-error: bytes_read == 512
        fprintf(stdout, "Failed to read first sector\n");
        fclose(image);
        return 1;
    }

    if(sector[510] != 0x55 || sector[511] != 0xAA){ // Invalid MBR signature
        fclose(image);
        return 1;
    }

    for(int i=0; i<4; i++){
        const uint8_t* entry = &sector[PARTITION_TABLE_OFFSET + i * PARTITION_ENTRY_SIZE];

        uint8_t type = entry[4];
        uint32_t start_sector = read_little_endian32(entry + 8);
        uint32_t sector_count = read_little_endian32(entry + 12);

        if(type == 0x00 || sector_count == 0){
            continue;
        }

        if(is_extended(type)){
            if (!parse_ebr(image, start_sector)) {
                fclose(image);
                return 1;
            }
            continue;
        }

        printf("%s,%u,%u\n", get_filesystem_type(type), (unsigned int)start_sector, (unsigned int)sector_count);
    }

    fclose(image);
}
