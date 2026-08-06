#include <stdio.h>
#include <stdint.h>

#define SECTOR_SIZE 512
#define PARTITION_TABLE_OFFSET 446
#define PARTITION_ENTRY_SIZE 16

void usage(){
    fprintf(stdout, "Usage: <evidence image>\n");
    return;
}

uint32_t read_little_endian32(const uint8_t* data){
    uint32_t d = ((uint32_t)data[0]) | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
    return d;
}

int main(int argc, char* argv[])
{
    FILE* image;
    uint8_t sector[SECTOR_SIZE];
    size_t bytes_reads;

    if(argc != 2){
        usage();
        return 1;
    }

    image = fopen(argv[1], "rb"); // open evidence image binary read
    if(image == NULL){ // failed to open file
        return 1;
    }

    bytes_reads = fread(sector, 1, SECTOR_SIZE, image); // read first sector, MBR

    if(bytes_reads != SECTOR_SIZE){ //  non-error: bytes_read == 512
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

        printf("Entry %d: TYPE=0x%02X, START=%u, SIZE=%u\n",
               i+1, type, (unsigned int)start_sector, (unsigned int)sector_count);
    }

    fclose(image);
}
