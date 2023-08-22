#ifndef NVM_INC
#define NVM_INC

//TODO can we do this dynamically?
#define NVM_PAGE_SIZE               1024        // # of 32-bit Instructions per Page
//#define NVM_BYTE_PAGE_SIZE          (4 * NVM_PAGE_SIZE) // Page size in Bytes
#define NVM_ROW_SIZE                32         // # of 32-bit Instructions per Row
#define NVM_BYTE_ROW_SIZE           (4 * NVM_ROW_SIZE) // # Row size in Bytes
#define NVM_NUM_ROWS_PAGE           8              //Number of Rows per Page 

typedef enum {NVM_OK, NVM_ERROR} NVM_Result_t;

//get start of flash page from a given pointer by rounding to NVM_PAGE_SIZE
#define NVM_GET_PAGE_START(X) (uint32_t*) (((uint32_t) X/NVM_PAGE_SIZE)*NVM_PAGE_SIZE)

void NVM_init();
NVM_Result_t NVM_memcpy4(void * dst, void * src, uint32_t length);
NVM_Result_t NVM_writeRow(void* address, void * data);
NVM_Result_t NVM_writeWord(void* address, unsigned int data);
NVM_Result_t NVM_erasePage(void* address);
NVM_Result_t NVM_writeToBufferedPage(uint8_t * dst, uint8_t * src, uint32_t length, uint32_t * writtenLength);
NVM_Result_t NVM_memsetBuffered(uint8_t * dst, uint8_t val, uint32_t length);
NVM_Result_t NVM_memcpyBuffered(uint8_t * dst, uint8_t * src, uint32_t length);
NVM_Result_t NVM_flush();

#endif