/*
 * File:   NVM.c
 * Author: Thorb
 *
 * Created on 12 January 2023, 21:00
 */


#include <xc.h>
#include <sys/kmem.h>
#include <string.h>

#include "FreeRTOS.h"
#include "NVM.h"
#include "HAL.h"
#include "UART32.h"


static uint8_t * pageBuffer;
static uint32_t * pagePointer;
static uint32_t pageStale;

void NVM_init(){
    pageBuffer = pvPortMalloc(NVM_PAGE_SIZE);
    pagePointer = 0;
}
 
static NVM_Result_t __attribute__((nomips16)) NVM_operation(unsigned int nvmop){
    int int_status;
    int susp;
    
    vTaskEnterCritical();
    
    HAL_clearMainOutput();
    
    //disable DMA
    susp = DMACONbits.ON;
    DMACONbits.ON = 0;
    while(DMACONbits.DMABUSY);
    
    NVMCON = NVMCON_WREN | nvmop;
    {
    unsigned long t0 = _CP0_GET_COUNT();
    while (_CP0_GET_COUNT() - t0 < (80/2)*6);
    }

    NVMKEY = 0xAA996655;
    NVMKEY = 0x556699AA;
    NVMCONSET = NVMCON_WR;
    
    while(NVMCON & NVMCON_WR);
    
    NVMCONCLR = NVMCON_WREN;
    
    //re-enable DMA
    DMACONbits.ON = susp;
    
    vTaskExitCritical();
    
    return ((NVMCON & (_NVMCON_LVDERR_MASK | _NVMCON_WRERR_MASK)) == 0) ? NVM_OK : NVM_ERROR;
}

NVM_Result_t NVM_memcpy4(void * dst, void * src, uint32_t length){
    uint32_t currOffset = 0;
    uint32_t time = _CP0_GET_COUNT();
    for(;currOffset < length; currOffset += 4){
        uint32_t * data = (uint32_t *) ((uint32_t) src + currOffset);
        
        //can we write more than one row of data and happen to be at the start of one?
        if((length - currOffset) >= NVM_BYTE_ROW_SIZE && (((uint32_t) dst + currOffset) % NVM_BYTE_ROW_SIZE) == 0){
            //yes :D we can write an entire row
            //UART_print("write row from 0x%08x\r\n", (uint32_t) dst + currOffset);
            if(NVM_writeRow(dst + currOffset, data) == NVM_ERROR) return NVM_ERROR;
            
            //skip all the bytes that got written in addition to the 4 that the loop is expecting
            currOffset += NVM_BYTE_ROW_SIZE - 4;
        }else{
            //no, not enough data left to get to a row
            //UART_print("write word to 0x%08x\r\n", (uint32_t) dst + currOffset);
            if(NVM_writeWord(dst + currOffset, *data) == NVM_ERROR) return NVM_ERROR;
        }
    }
    time = (_CP0_GET_COUNT() - time) / 48000;
    //UART_print("write time = %d ms (=%d bytes/ms)\r\n", time, length / time);
    return NVM_OK;
}

NVM_Result_t NVM_writeRow(void* address, void * data){
    NVMADDR = KVA_TO_PA((unsigned int) address);
    NVMSRCADDR = KVA_TO_PA((unsigned int) data);
    return NVM_operation(0x4003); //NVM Row Program
}

NVM_Result_t NVM_writeWord(void* address, unsigned int data){
    NVMADDR = KVA_TO_PA((unsigned int)address);
    NVMDATA = data;
    return NVM_operation(0x4001);
}

NVM_Result_t NVM_erasePage(void* address){
    NVMADDR = KVA_TO_PA((unsigned int)address);
    return NVM_operation(0x4004);
}

static NVM_Result_t checkAndRebuffer(void* address){
    //sanity check the address. If this is not in flash we obviously don't want to do anything
    if(address != NULL && ((uint32_t) address < __KSEG0_PROGRAM_MEM_BASE || (uint32_t) address >= __KSEG0_PROGRAM_MEM_BASE + __KSEG0_PROGRAM_MEM_LENGTH)) return NVM_ERROR;
    
    //check if the page currently buffered contains the requested address, if not we load the page corresponding to that
    if(NVM_GET_PAGE_START(address) != pagePointer || address == NULL){
        //no! flush the last page to flash and load the next one

        //check if anything was changed
        if(pagePointer != NULL && memcmp(pagePointer, pageBuffer, NVM_PAGE_SIZE) != 0){
            uint32_t eraseRequired = 1; //TODO revert this to 0 and verify the compare operation. Or is that unnecessary overkill?
            
            //check if we need to erase the current flash page. This is only necessary if bit positions will be set that weren't before
            /*for(uint32_t i = 0; i < NVM_PAGE_SIZE; i+=4){
                //check if at least one bit that is zero will be one after the write
                if(((~(*pagePointer)) & (*pageBuffer)) != 0){
                    //yes...
                    eraseRequired = 1;
                    break;
                }
            }*/
            
            vTaskEnterCritical();
            
            //there is unsaved data in the buffer, flush it. First erase the page, then write the new data to it
            
            if(eraseRequired){
                if(NVM_erasePage(pagePointer) == NVM_ERROR){ 
                    vTaskExitCritical();
                    return NVM_ERROR;
                }
            }
            
            if(pagePointer != NULL) if(NVM_memcpy4(pagePointer, pageBuffer, NVM_PAGE_SIZE) == NVM_ERROR){ 
                vTaskExitCritical();
                return NVM_ERROR;
            }
            vTaskExitCritical();
        }
        
        //did we just flush data or are we rebuffering a new page?
        if(address != NULL){
            //at this point the data in the buffer is no longer needed, overwrite it with the new stuff
            pagePointer = NVM_GET_PAGE_START(address);
            memcpy(pageBuffer, pagePointer, NVM_PAGE_SIZE);
        }
    }
    return NVM_OK;
}

//TODO: move operation might run itself over when dst is higher in memory than the src and crosses a page boundary
NVM_Result_t NVM_writeToBufferedPage(uint8_t * dst, uint8_t * src, uint32_t length, uint32_t * writtenLength){
    uint8_t * realSrc = src;
    uint32_t srcSize = length;

    uint8_t * realDst = dst;
    uint32_t dstSize = length;
    
    //is the destination in flash?
    if((uint32_t) dst >= __KSEG0_PROGRAM_MEM_BASE && (uint32_t) dst < __KSEG0_PROGRAM_MEM_BASE + __KSEG0_PROGRAM_MEM_LENGTH){
        //yes! We need to make sure we have the page we want to write buffered
        if(checkAndRebuffer(dst) != NVM_OK) return NVM_ERROR;
        
        //we also need to update the dst address to the buffer in ram, and update the size to the bytes left to write
        uint32_t writeOffset = (uint32_t) dst - (uint32_t) pagePointer;
        realDst = &pageBuffer[writeOffset];
        dstSize = NVM_PAGE_SIZE - writeOffset;
        
    }   //no. No need to buffer anything, the write is going into ram anyway. Also no need to change the dst pointer

    //is the source currently within the buffered area? WHERE'S THE LAMB SOURCE???????
    if(NVM_GET_PAGE_START(src) == pagePointer){
        //yes, true src for next op will be ram
        uint32_t readOffset = (uint32_t) src - (uint32_t) pagePointer;

        //get pointer to start of src in buffered page
        realSrc = &pageBuffer[readOffset];

        //update maximum number of bytes readable as bytes until end of buffered page
        srcSize = NVM_PAGE_SIZE - readOffset;
    }else if(NVM_GET_PAGE_START(src) < pagePointer){
        //no, but we need to make sure we don't move into the buffered region as the copy progresses. Possible because the buffered page lies after the src
        //update the src size to equal the remaining number of bytes until buffer start
        srcSize = (uint32_t) pagePointer - (uint32_t) src;
    }else ; //nope, src is behind the region we have in the buffer now, so it doesn't matter what we do with it. We can just read normally

    //how many bytes can we copy until either the dst or src moves out of the buffer?
    uint32_t writeLength = (srcSize > dstSize) ? dstSize : srcSize;
    if(writeLength > length) writeLength = length;
    
    //write that data to the dst (and use memmove instead of memcpy as it can handle overlapping regions)
    memmove(realDst, realSrc, writeLength);
    

    *writtenLength = writeLength;
    
    return NVM_OK;
}

NVM_Result_t NVM_flush(){
    //flush any changes remaining in the buffer to flash
    checkAndRebuffer(NULL);
    return NVM_OK;
}

NVM_Result_t NVM_memcpyBuffered(uint8_t * dst, uint8_t * src, uint32_t length){
    uint32_t writtenBytes = 0;
    uint32_t originalLength = length;
    
    while(length > 0){
        if(NVM_writeToBufferedPage(dst, src, length, &writtenBytes) == NVM_ERROR) return NVM_ERROR;

        dst += writtenBytes;
        src += writtenBytes;
        length -= writtenBytes;
        
        //UART_print("written %d of %d bytes to the buffer / flash\r\n", writtenBytes, originalLength);
    }
    
    return NVM_OK;
}

NVM_Result_t NVM_memsetBuffered(uint8_t * dst, uint8_t val, uint32_t length){
    uint32_t writtenBytes = 0;
    
    while(length > 0){
        if(NVM_writeToBufferedPage(dst, &val, 1, &writtenBytes) == NVM_ERROR) return NVM_ERROR;

        dst += writtenBytes;
        length -= writtenBytes;
    }
    
    return NVM_OK;
}