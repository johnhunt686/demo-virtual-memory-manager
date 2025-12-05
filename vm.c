#include <stdio.h>
#include <stdlib.h>

#define BUFFER_SIZE         10 
#define PAGE_TABLE_SIZE     256
#define NUMBER_OF_FRAMES    128 //256 initially 
#define TLB_SIZE            16 

//settings
#define DEBUG               0
#define USE_LRU_TLB         1 //0 for FIFO
#define USE_LRU_PAGE_T      1 //0 for FIFO

/* an entry in the tlb */  
struct tlb_entry 
{ 
    int page_number; 
    int frame_number; 
    int valid;
    int age; // LRU ccounter
}; 

FILE *address_file; 
FILE *backing_store; 

/* buffer for reading logical addresses */ 
char buffer[BUFFER_SIZE]; 

/* representation of physical memory */ 
signed char *physical_memory; 

/* page table */ 
int page_table[PAGE_TABLE_SIZE]; 

/* list of free frames */ 
int free_frame_list[NUMBER_OF_FRAMES]; 

/* index of next free frame */
int next_free_frame;

/* index of next open tlb entry */
int tlb_next;

/* global timestamp for LRU */
int elapsed_time;

/*FIFO frame to be next removed*/
int next_frame_to_evict = 0;

/*LRU frame age array*/
int frame_age[NUMBER_OF_FRAMES];

/* the tlb */ 
struct tlb_entry tlb[TLB_SIZE];

int tlb_lookup(int page_number){
    for (int i=0 ; i<TLB_SIZE;i++){
        if (tlb[i].valid && tlb[i].page_number == page_number){
            tlb[i].age = elapsed_time; //update age
            return tlb[i].frame_number;
        }
    }
    return -1; //miss
}

void tlb_insert(int page_number, int frame_number, int *tlb_next){
    tlb[*tlb_next].valid = 1;
    tlb[*tlb_next].page_number = page_number;
    tlb[*tlb_next].frame_number = frame_number;
    //increment tlb_next (circular)
    *tlb_next = (*tlb_next + 1 )% TLB_SIZE;
}

void tlb_insert_lru(int page_number, int frame_number){
    int lowest_age = __INT32_MAX__;
    int lowest_age_index = 0;
    //look for invalid entry first
    for (int i=0; i<TLB_SIZE; i++){
        if (tlb[i].valid == 0){ //invalid, replace
            tlb[i].valid = 1;
            tlb[i].page_number = page_number;
            tlb[i].frame_number = frame_number;
            tlb[i].age = elapsed_time;
            return;
        }
        //update lowest age while we're sweeping
        if (tlb[i].age < lowest_age){
            lowest_age = tlb[i].age;
            lowest_age_index = i;
        }
    }

    //table is full, replace oldest entry
    tlb[lowest_age_index].valid = 1;
    tlb[lowest_age_index].page_number = page_number;
    tlb[lowest_age_index].frame_number = frame_number;
    tlb[lowest_age_index].age = elapsed_time;
}

/*chooses a frame to evict based on FIFO*/
int select_victim_frame_fifo() {
    int victim = next_frame_to_evict;
    next_frame_to_evict = (next_frame_to_evict + 1) % NUMBER_OF_FRAMES;
    return victim;
}

/*chooses a frame to evict based on LRU(age)*/
int select_victim_frame_lru() {
    int oldest_frame = 0;
    int oldest_age = frame_age[0];

    for (int i = 1; i < NUMBER_OF_FRAMES; i++) {
        if (frame_age[i] < oldest_age) {
            oldest_age = frame_age[i];
            oldest_frame = i;
        }
    }

    return oldest_frame;
}

void invalidate_frame(int victim_frame) {
    //find the page that maps this frame
    for (int page=0; page<PAGE_TABLE_SIZE; page++){
        if (page_table[page] == victim_frame) {
            page_table[page] = -1;

            //invalidate corresponding tlb
            for (int i=0; i<TLB_SIZE; i++){
                if (tlb[i].valid && tlb[i].page_number == page){
                    tlb[i].valid = 0;
                }
            }
            break; //successfully invalidated
        }
    }
}

int main(int argc, char *argv[]) { 
    //statistics
    int page_faults = 0;
    int TLB_hits = 0;
    int total_access = 0;

    /* 1. open the file containing the backing store */ 
    backing_store = fopen(argv[1], "rb");
    address_file = fopen(argv[2], "r");
    if (backing_store == NULL) {
        fprintf(stderr, "Unable to open file '%s'!\n", argv[1]);
        return 1;
    }
    if (address_file == NULL) {
        fprintf(stderr, "Unable to open file '%s'!\n", argv[2]);
        return 1;
    }

    /* 2. allocate physical memory */ 
    physical_memory = malloc(65536);

    /* 3. initialize the page table */
    for (int i=0;i<PAGE_TABLE_SIZE;i++){
        page_table[i]= -1;
    }
    for (int i=0;i<NUMBER_OF_FRAMES;i++){
        free_frame_list[i] = 0;
    }
    next_free_frame = 0;
    //page replacement
    for (int i=0;i<NUMBER_OF_FRAMES;i++){
        frame_age[i]=0;
    }

    /* 4. initially flush the tlb */ 
    tlb_next = 0;
    for (int i=0; i<TLB_SIZE; i++){
        tlb[i].valid = 0;
    }
    
    /* 5. read through the input file and translate each logical address  
    to its corresponding physical address, and extract the byte value 
    (represented as a signed char) at the physical address.*/
    while (fgets(buffer, sizeof(buffer), address_file)){ //read in a logical address 
        //pre-loop
        elapsed_time++;

        //extract the page number and offset from the logical address
        int logical_address = atoi(buffer);
        int page_number = (logical_address >> 8) & 0xFF;
        int offset = logical_address & 0xFF;

        if (DEBUG){
            printf("Logical: %d  Page: %d  Offset: %d\n",
            logical_address, page_number, offset);
        } else {
            printf("Logical: %d   ",
            logical_address);
        }

        total_access++;
        //try to get frame from tlb
        int frame_number = tlb_lookup(page_number);

        if(frame_number != -1){
            TLB_hits++;
        } else {
            //try to get frame number from page table
            frame_number = page_table[page_number];
            //check if page_table hit
            if (frame_number == -1){ // page table miss
                page_faults++;
                //handle full frames
                if (next_free_frame < NUMBER_OF_FRAMES){
                    frame_number = next_free_frame;
                    next_free_frame++;
                } else {
                    frame_number = (USE_LRU_PAGE_T) ? select_victim_frame_lru():select_victim_frame_fifo();
                    invalidate_frame(frame_number);
                }
                
                //seek to correct page in backing store
                fseek(backing_store, page_number * 256, SEEK_SET); 
                //read page into physical memory
                fread(physical_memory + frame_number * 256, 1, 256, backing_store);
                //update page table
                page_table[page_number] = frame_number;
                //update free frame list
                free_frame_list[frame_number] = 1;
            }
            //insert into tlb after finding
            if (USE_LRU_TLB) {
                tlb_insert_lru(page_number, frame_number);
            } else {
                tlb_insert(page_number, frame_number, &tlb_next);
}

        }


        int physical_address = frame_number * 256 + offset;
        signed char value = physical_memory[physical_address];
        if (DEBUG) {
            printf("Physical Address: %d   Value: %d\n", 
                physical_address, value);
        } else {
            printf("Physical: %d    Value: %d\n", 
                physical_address, value);
        }
    }
    
    
    /* 6. print out the statistics */

    float page_fault_rate =  100 * page_faults / total_access;
    float tlb_hit_rate = 100 * TLB_hits / total_access;
    printf("\nPage Fault Rate: %.1f%%    TLB Hit Rate: %.1f%%\n",
        page_fault_rate,tlb_hit_rate);

    /* 7. close file resources */
    //TODO
    fclose(backing_store);
    fclose(address_file);
    free(physical_memory);

    return 0;
}
