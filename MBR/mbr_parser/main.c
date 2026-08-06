#include <stdio.h>
#include <stdint.h>

#define SECTOR_SIZE 512

void usage(){
    fprintf(stdout, "Usage: %s <evidence image>\n", argv[0]);
    return;
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

    bytes_read = fread(sector, 1, SECTOR_SIZE, image); // read first sector, MBR

    if(bytes_read != SECTOR_SIZE){ //  non-error: bytes_read == 512
        fprintf(stdout, "failed open file\n");
        fclose(image);
        return 1;
    }

    printf("good");
    fclose(image);
}
