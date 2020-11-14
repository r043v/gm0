#include <stdio.h>
#include <stdint.h>

#define BLOCK_COUNT 7
#define BLOCK_LENGTH 4
#define BLOCK_TOTAL (BLOCK_COUNT*BLOCK_LENGTH)

typedef unsigned char byte_t;

int main(int argc, char *argv[])
{
  FILE* pf;
  byte_t buf[BLOCK_TOTAL];
  uint32_t crc = 0;
  uint32_t block;
  
  // Check for required arguments
  if (argc < 2)
  {
    printf("syntax: lpcrc <firmware.bin>\n");
    return 1;
  }
  
  // Try to open the supplied firmware
  if ((pf = fopen(argv[1],"rb+")) == NULL)
  {
    printf("error: could not open file [%s] with write access\n",argv[1]);
    return 1;
  }

  // Read out the data blocks used for crc calculation
  if (fread(buf,1,BLOCK_TOTAL,pf) != BLOCK_TOTAL)
  {
    printf("error: could not read required bytes\n");
    fclose(pf);
    return 1;
  }

  // Compute the crc value
  for (block=0; block<BLOCK_COUNT; block++)
  {
    crc += *((uint32_t*)(buf+(block*BLOCK_LENGTH)));
  }
  crc = (~crc) + 1;
  
  // Reposition the file stream indicator to switch between read and write
  if (fseek(pf,0,SEEK_CUR) != 0)
  {
    printf("error: could not switch from read to write mode\n");
    fclose(pf);
    return 1;
  }
  
  // Write the crc back to the file
  if (fwrite((byte_t*)&crc,1,BLOCK_LENGTH,pf) != BLOCK_LENGTH)
  {
    printf("error: could not write crc back to file\n");
    fclose(pf);
    return 1;
  }

  printf("succesfully updated crc to: %08x\n",crc);
  fclose(pf);
  
  return 0;
}
