/***********************************************************************
*
*  FILE        : boot_loader.c
*  DATE        : 2019-05-18
*  DESCRIPTION : Main Program
*
*  NOTE:THIS IS A TYPICAL EXAMPLE.
*
***********************************************************************/
#include <stdio.h>
#include <string.h>
#include "r_smc_entry.h"
#include "r_flash_rx_if.h"
#include "r_sci_rx_if.h"
#include "r_cryptogram.h"

#include "r_sci_rx_pinset.h"

/*------------------------------------------ firmware update configuration (start) --------------------------------------------*/
/* R_FLASH_Write() arguments: specify "low address" and process to "high address" */
#define BOOT_LOADER_LOW_ADDRESS FLASH_CF_BLOCK_13
#define BOOT_LOADER_MIRROR_LOW_ADDRESS FLASH_CF_BLOCK_51

/* R_FLASH_Erase() arguments: specify "high address (low block number)" and process to "low address (high block number)" */
#define BOOT_LOADER_MIRROR_HIGH_ADDRESS FLASH_CF_BLOCK_38
#define BOOT_LOADER_UPDATE_TEMPORARY_AREA_HIGH_ADDRESS FLASH_CF_BLOCK_52

#define BOOT_LOADER_MIRROR_BLOCK_NUM_FOR_SMALL 8
#define BOOT_LOADER_MIRROR_BLOCK_NUM_FOR_MEDIUM 6

#define BOOT_LOADER_USER_CONST_DATA_LOW_ADDRESS FLASH_DF_BLOCK_0
#define BOOT_LOADER_CONST_DATA_BLOCK_NUM 0

#define BOOT_LOADER_USER_FIRMWARE_HEADER_LENGTH 0x200
#define BOOT_LOADER_USER_FIRMWARE_DESCRIPTOR_LENGTH 0x100
#define INITIAL_FIRMWARE_FILE_NAME "userprog.rsu"

#define FLASH_INTERRUPT_PRIORITY 14	/* 0(low) - 15(high) */
#define SCI_INTERRUPT_PRIORITY 15	/* 0(low) - 15(high) */

/*------------------------------------------ firmware update configuration (end) --------------------------------------------*/


#define BOOT_LOADER_UPDATE_TEMPORARY_AREA_LOW_ADDRESS FLASH_CF_LO_BANK_LO_ADDR
#define BOOT_LOADER_UPDATE_EXECUTE_AREA_LOW_ADDRESS FLASH_CF_HI_BANK_LO_ADDR
#define BOOT_LOADER_UPDATE_TARGET_BLOCK_NUMBER (FLASH_NUM_BLOCKS_CF - BOOT_LOADER_MIRROR_BLOCK_NUM_FOR_SMALL - BOOT_LOADER_MIRROR_BLOCK_NUM_FOR_MEDIUM)
#define BOOT_LOADER_UPDATE_CONST_DATA_TARGET_BLOCK_NUMBER (FLASH_NUM_BLOCKS_DF - BOOT_LOADER_CONST_DATA_BLOCK_NUM)
#define USER_RESET_VECTOR_ADDRESS (BOOT_LOADER_LOW_ADDRESS - 4)

#define BOOT_LOADER_SUCCESS         (0)
#define BOOT_LOADER_FAIL            (-1)
#define BOOT_LOADER_GOTO_INSTALL    (-2)
#define BOOT_LOADER_IN_PROGRESS     (-3)

#define BOOT_LOADER_STATE_INITIALIZING								1
#define BOOT_LOADER_STATE_BANK1_CHECK								2
#define BOOT_LOADER_STATE_BANK1_UPDATE_LIFECYCLE_ERASE_WAIT			3
#define BOOT_LOADER_STATE_BANK1_UPDATE_LIFECYCLE_ERASE_COMPLETE		4
#define BOOT_LOADER_STATE_BANK1_UPDATE_LIFECYCLE_WRITE_WAIT			5
#define BOOT_LOADER_STATE_BANK1_UPDATE_LIFECYCLE_WRITE_COMPLETE		6
#define BOOT_LOADER_STATE_BANK0_CHECK								7
#define BOOT_LOADER_STATE_BANK0_INSTALL_SECURE_BOOT_ERASE_WAIT		8
#define BOOT_LOADER_STATE_BANK0_INSTALL_SECURE_BOOT_ERASE_COMPLETE	9
#define BOOT_LOADER_STATE_BANK0_INSTALL_SECURE_BOOT_WRITE_WAIT1		10
#define BOOT_LOADER_STATE_BANK0_INSTALL_SECURE_BOOT_WRITE_COMPLETE1	11
#define BOOT_LOADER_STATE_BANK0_INSTALL_SECURE_BOOT_WRITE_WAIT2		12
#define BOOT_LOADER_STATE_BANK0_INSTALL_SECURE_BOOT_WRITE_COMPLETE2	13
#define BOOT_LOADER_STATE_BANK0_INSTALL_DATA_FLASH_ERASE_WAIT		14
#define BOOT_LOADER_STATE_BANK0_INSTALL_DATA_FLASH_ERASE_COMPLETE	15
#define BOOT_LOADER_STATE_BANK0_INSTALL_CODE_FLASH_ERASE_WAIT		16
#define BOOT_LOADER_STATE_BANK0_INSTALL_CODE_FLASH_ERASE_COMPLETE	17
#define BOOT_LOADER_STATE_BANK0_INSTALL_CODE_FLASH_READ_WAIT		18
#define BOOT_LOADER_STATE_BANK0_INSTALL_CODE_FLASH_READ_COMPLETE	19
#define BOOT_LOADER_STATE_BANK0_INSTALL_CODE_FLASH_WRITE_WAIT		20
#define BOOT_LOADER_STATE_BANK0_INSTALL_CODE_FLASH_WRITE_COMPLETE	21
#define BOOT_LOADER_STATE_BANK0_INSTALL_DATA_FLASH_WRITE_WAIT		22
#define BOOT_LOADER_STATE_BANK0_INSTALL_DATA_FLASH_WRITE_COMPLETE	23
#define BOOT_LOADER_STATE_FATAL_ERROR								200

#define LIFECYCLE_STATE_BLANK		(0xff)
#define LIFECYCLE_STATE_TESTING		(0xfe)
#define LIFECYCLE_STATE_VALID		(0xfc)
#define LIFECYCLE_STATE_INVALID		(0xf8)

#define MAX_CHECK_DATAFLASH_AREA_RETRY_COUNT 3
#define SHA1_HASH_LENGTH_BYTE_SIZE 20


typedef struct _load_firmware_control_block {
    uint32_t flash_buffer[FLASH_CF_MEDIUM_BLOCK_SIZE / 4];
    uint32_t offset;
    uint32_t progress;
}LOAD_FIRMWARE_CONTROL_BLOCK;

typedef struct _load_const_data_control_block {
    uint32_t flash_buffer[FLASH_DF_BLOCK_SIZE / 4];
    uint32_t offset;
    uint32_t progress;
}LOAD_CONST_DATA_CONTROL_BLOCK;

typedef struct _sci_receive_control_block {
   uint8_t buffer[FLASH_CF_MEDIUM_BLOCK_SIZE];
   volatile  uint32_t buffer_occupied_byte_size;
   uint32_t total_byte_size;
}SCI_RECEIVE_CONTROL_BLOCK;

typedef struct _firmware_update_control_block
{
	uint8_t magic_code[7];
    uint8_t image_flag;
    uint8_t signature_type[32];
    uint32_t signature_size;
    uint8_t signature[256];
    uint32_t dataflash_flag;
    uint32_t dataflash_start_address;
    uint32_t dataflash_end_address;
    uint8_t reserved1[200];
    uint32_t sequence_number;
    uint32_t start_address;
    uint32_t end_address;
    uint32_t execution_address;
    uint32_t hardware_id;
    uint8_t reserved2[236];
}FIRMWARE_UPDATE_CONTROL_BLOCK;

void main(void);
static int32_t secure_boot(void);
static int32_t firm_block_read(uint32_t *firmware, uint32_t offset);
static int32_t const_data_block_read(uint32_t *const_data, uint32_t offset);
static void bank_swap_with_software_reset(void);
static void software_reset(void);
static const uint8_t *get_status_string(uint8_t status);
static void my_sci_callback(void *pArgs);
static void my_flash_callback(void *event);

extern void my_sw_charget_function(void);
extern void my_sw_charput_function(uint8_t data);

static FIRMWARE_UPDATE_CONTROL_BLOCK *firmware_update_control_block_bank0 = (FIRMWARE_UPDATE_CONTROL_BLOCK*)BOOT_LOADER_UPDATE_EXECUTE_AREA_LOW_ADDRESS;
static FIRMWARE_UPDATE_CONTROL_BLOCK *firmware_update_control_block_bank1 = (FIRMWARE_UPDATE_CONTROL_BLOCK*)BOOT_LOADER_UPDATE_TEMPORARY_AREA_LOW_ADDRESS;
static LOAD_FIRMWARE_CONTROL_BLOCK load_firmware_control_block;
static LOAD_CONST_DATA_CONTROL_BLOCK load_const_data_control_block;
static uint32_t secure_boot_state = BOOT_LOADER_STATE_INITIALIZING;
static uint32_t flash_error_code;

/* Handle storage. */
sci_hdl_t     my_sci_handle;
SCI_RECEIVE_CONTROL_BLOCK sci_receive_control_block;

void main(void)
{
    int32_t result_secure_boot;
    nop();
    while(1)
    {
		result_secure_boot = secure_boot();
		if (BOOT_LOADER_SUCCESS == result_secure_boot)
		{
			/* stop all interrupt completely */
			set_psw(0);
			uint32_t addr;
			addr = *(uint32_t*)USER_RESET_VECTOR_ADDRESS;
			((void (*)())addr)();
			while(1); /* infinite loop */
		}
		else if (BOOT_LOADER_FAIL == result_secure_boot)
		{
			while(1)
			{
				/* infinity loop */
			}
		}
		else if (BOOT_LOADER_IN_PROGRESS == result_secure_boot)
		{
			continue;
		}
		else
		{
			while(1)
			{
				/* infinite loop */
			}
		}
    }
}

static int32_t secure_boot(void)
{
    flash_err_t flash_api_error_code = FLASH_SUCCESS;
    int32_t secure_boot_error_code = BOOT_LOADER_IN_PROGRESS;
    uint8_t hash_sha1[SHA1_HASH_LENGTH_BYTE_SIZE];
    uint32_t bank_info = 255;
    flash_interrupt_config_t cb_func_info;
	FIRMWARE_UPDATE_CONTROL_BLOCK *firmware_update_control_block_tmp = (FIRMWARE_UPDATE_CONTROL_BLOCK*)load_firmware_control_block.flash_buffer;

    switch(secure_boot_state)
    {
    	case BOOT_LOADER_STATE_INITIALIZING:
    	    R_SCI_PinSet_SCI8();

    	    sci_cfg_t   my_sci_config;
    	    sci_err_t   my_sci_err;

    	    /* Set up the configuration data structure for asynchronous (UART) operation. */
    	    my_sci_config.async.baud_rate    = 115200;
    	    my_sci_config.async.clk_src      = SCI_CLK_INT;
    	    my_sci_config.async.data_size    = SCI_DATA_8BIT;
    	    my_sci_config.async.parity_en    = SCI_PARITY_OFF;
    	    my_sci_config.async.parity_type  = SCI_EVEN_PARITY;
    	    my_sci_config.async.stop_bits    = SCI_STOPBITS_1;
    	    my_sci_config.async.int_priority = SCI_INTERRUPT_PRIORITY;

    	    /* OPEN ASYNC CHANNEL
    	    *  Provide address of the configure structure,
    	    *  the callback function to be assigned,
    	    *  and the location for the handle to be stored.*/
    	    my_sci_err = R_SCI_Open(SCI_CH8, SCI_MODE_ASYNC, &my_sci_config, my_sci_callback, &my_sci_handle);

    	    /* If there were an error this would demonstrate error detection of API calls. */
    	    if (SCI_SUCCESS != my_sci_err)
    	    {
    	        nop(); // Your error handling code would go here.
				secure_boot_state = BOOT_LOADER_STATE_FATAL_ERROR;
				secure_boot_error_code = BOOT_LOADER_FAIL;
				break;
    	    }

    	    load_firmware_control_block.progress = 0;
    	    load_firmware_control_block.offset = 0;

    	    flash_api_error_code = R_FLASH_Open();
    	    if (FLASH_SUCCESS == flash_api_error_code)
    	    {
    	        /* nothing to do */
    	    }
    	    else
    	    {
    	        printf("R_FLASH_Open() returns error. %d.\r\n", flash_error_code);
    	        printf("system error.\r\n");
				secure_boot_state = BOOT_LOADER_STATE_FATAL_ERROR;
				secure_boot_error_code = BOOT_LOADER_FAIL;
    	    }

    	    /* startup system */
    	    printf("-------------------------------------------------\r\n");
    	    printf("RX65N secure boot program\r\n");
    	    printf("-------------------------------------------------\r\n");

    	    printf("Checking flash ROM status.\r\n");

    	    printf("bank 0 status = 0x%x [%s]\r\n", firmware_update_control_block_bank0->image_flag, get_status_string(firmware_update_control_block_bank0->image_flag));
    	    printf("bank 1 status = 0x%x [%s]\r\n", firmware_update_control_block_bank1->image_flag, get_status_string(firmware_update_control_block_bank1->image_flag));

    	    R_FLASH_Control(FLASH_CMD_BANK_GET, &bank_info);
    	    printf("bank info = %d. (start bank = %d)\r\n", bank_info, (bank_info ^ 0x01));

    		cb_func_info.pcallback = my_flash_callback;
    		cb_func_info.int_priority = FLASH_INTERRUPT_PRIORITY;
    	    R_FLASH_Control(FLASH_CMD_SET_BGO_CALLBACK, (void *)&cb_func_info);
    	    secure_boot_state = BOOT_LOADER_STATE_BANK1_CHECK;
    		break;

    	case BOOT_LOADER_STATE_BANK1_CHECK:
    		if(firmware_update_control_block_bank1->image_flag == LIFECYCLE_STATE_TESTING)
    		{
    	    	memcpy(load_firmware_control_block.flash_buffer, (void*)BOOT_LOADER_UPDATE_TEMPORARY_AREA_LOW_ADDRESS, FLASH_CF_MEDIUM_BLOCK_SIZE);

    	    	printf("bank1(temporary area) on code flash hash check...");
    	        R_Sha1((uint8_t*)BOOT_LOADER_UPDATE_TEMPORARY_AREA_LOW_ADDRESS + BOOT_LOADER_USER_FIRMWARE_HEADER_LENGTH,
    	        		hash_sha1, (FLASH_CF_MEDIUM_BLOCK_SIZE * BOOT_LOADER_UPDATE_TARGET_BLOCK_NUMBER) - BOOT_LOADER_USER_FIRMWARE_HEADER_LENGTH);
    	        if(0 == memcmp(hash_sha1, firmware_update_control_block_bank1->signature, SHA1_HASH_LENGTH_BYTE_SIZE))
    	        {
    	            printf("OK\r\n");
    	        	firmware_update_control_block_tmp->image_flag = LIFECYCLE_STATE_VALID;
    	        }
    	        else
    	        {
    	            printf("NG\r\n");
    	        	firmware_update_control_block_tmp->image_flag = LIFECYCLE_STATE_INVALID;
    	        }
    	    	printf("update LIFECYCLE_STATE from [%s] to [%s]\r\n", get_status_string(firmware_update_control_block_bank1->image_flag), get_status_string(firmware_update_control_block_tmp->image_flag));
    	    	printf("bank1(temporary area) block0 erase (to update LIFECYCLE_STATE)...");
    	    	flash_api_error_code = R_FLASH_Erase((flash_block_address_t)BOOT_LOADER_UPDATE_TEMPORARY_AREA_LOW_ADDRESS, 1);
    	        if (FLASH_SUCCESS != flash_api_error_code)
    	        {
    	            printf("R_FLASH_Erase() returns error. %d.\r\n", flash_error_code);
    	            printf("system error.\r\n");
					secure_boot_state = BOOT_LOADER_STATE_FATAL_ERROR;
					secure_boot_error_code = BOOT_LOADER_FAIL;
    	            break;
    	        }
    			secure_boot_state = BOOT_LOADER_STATE_BANK1_UPDATE_LIFECYCLE_ERASE_WAIT;
    		}
    		else
    		{
    			secure_boot_state = BOOT_LOADER_STATE_BANK0_CHECK;
    		}
			break;

    	case BOOT_LOADER_STATE_BANK1_UPDATE_LIFECYCLE_ERASE_WAIT:
    		/* this state will be update by flash callback */
    		break;

    	case BOOT_LOADER_STATE_BANK1_UPDATE_LIFECYCLE_ERASE_COMPLETE:
	        if (FLASH_SUCCESS == flash_error_code)
	        {
	            printf("OK\r\n");
	        }
	        else
	        {
	            printf("R_FLASH_Erase() callback error. %d.\r\n", flash_error_code);
	            printf("system error.\r\n");
	            secure_boot_state = BOOT_LOADER_STATE_FATAL_ERROR;
	            break;
	        }
	        printf("bank1(temporary area) block0 write (to update LIFECYCLE_STATE)...");
	        flash_api_error_code = R_FLASH_Write((uint32_t)firmware_update_control_block_tmp, (uint32_t)BOOT_LOADER_UPDATE_TEMPORARY_AREA_LOW_ADDRESS, FLASH_CF_MEDIUM_BLOCK_SIZE);
			if (FLASH_SUCCESS != flash_api_error_code)
			{
				printf("R_FLASH_Write() returns error. %d.\r\n", flash_error_code);
				printf("system error.\r\n");
				secure_boot_state = BOOT_LOADER_STATE_FATAL_ERROR;
				secure_boot_error_code = BOOT_LOADER_FAIL;
				break;
			}
			secure_boot_state = BOOT_LOADER_STATE_BANK1_UPDATE_LIFECYCLE_WRITE_WAIT;
			break;

    	case BOOT_LOADER_STATE_BANK1_UPDATE_LIFECYCLE_WRITE_WAIT:
    		/* this state will be update by flash callback */
    		break;

    	case BOOT_LOADER_STATE_BANK1_UPDATE_LIFECYCLE_WRITE_COMPLETE:
	        if (FLASH_SUCCESS == flash_error_code)
	        {
	            printf("OK\r\n");
	        }
	        else
	        {
	            printf("R_FLASH_Write() callback error. %d.\r\n", flash_error_code);
	            printf("system error.\r\n");
				secure_boot_state = BOOT_LOADER_STATE_FATAL_ERROR;
				secure_boot_error_code = BOOT_LOADER_FAIL;
	            break;
	        }
	        printf("swap bank...\r\n");
			R_BSP_SoftwareDelay(3000, BSP_DELAY_MILLISECS);
			bank_swap_with_software_reset();
			while(1);
			break;

    	case BOOT_LOADER_STATE_BANK0_CHECK:
    	case BOOT_LOADER_STATE_BANK0_INSTALL_SECURE_BOOT_ERASE_WAIT:
    	case BOOT_LOADER_STATE_BANK0_INSTALL_SECURE_BOOT_ERASE_COMPLETE:
    	case BOOT_LOADER_STATE_BANK0_INSTALL_SECURE_BOOT_WRITE_WAIT1:
    	case BOOT_LOADER_STATE_BANK0_INSTALL_SECURE_BOOT_WRITE_COMPLETE1:
    	case BOOT_LOADER_STATE_BANK0_INSTALL_SECURE_BOOT_WRITE_WAIT2:
    	case BOOT_LOADER_STATE_BANK0_INSTALL_SECURE_BOOT_WRITE_COMPLETE2:
    	case BOOT_LOADER_STATE_BANK0_INSTALL_DATA_FLASH_ERASE_WAIT:
    	case BOOT_LOADER_STATE_BANK0_INSTALL_DATA_FLASH_ERASE_COMPLETE:
    	case BOOT_LOADER_STATE_BANK0_INSTALL_CODE_FLASH_ERASE_WAIT:
    	case BOOT_LOADER_STATE_BANK0_INSTALL_CODE_FLASH_ERASE_COMPLETE:
    	case BOOT_LOADER_STATE_BANK0_INSTALL_CODE_FLASH_READ_WAIT:
    	case BOOT_LOADER_STATE_BANK0_INSTALL_CODE_FLASH_READ_COMPLETE:
    	case BOOT_LOADER_STATE_BANK0_INSTALL_CODE_FLASH_WRITE_WAIT:
    	case BOOT_LOADER_STATE_BANK0_INSTALL_CODE_FLASH_WRITE_COMPLETE:
    	case BOOT_LOADER_STATE_BANK0_INSTALL_DATA_FLASH_WRITE_WAIT:
    	case BOOT_LOADER_STATE_BANK0_INSTALL_DATA_FLASH_WRITE_COMPLETE:
    	    switch(firmware_update_control_block_bank0->image_flag)
    	    {
    	        case LIFECYCLE_STATE_BLANK:
    	        	switch(secure_boot_state)
					{
    	        		case BOOT_LOADER_STATE_BANK0_CHECK:
							printf("start installing user program.\r\n");
							printf("erase bank1 secure boot mirror area...");
							flash_api_error_code = R_FLASH_Erase(BOOT_LOADER_MIRROR_HIGH_ADDRESS, BOOT_LOADER_MIRROR_BLOCK_NUM_FOR_SMALL + BOOT_LOADER_MIRROR_BLOCK_NUM_FOR_MEDIUM);
							if(FLASH_SUCCESS != flash_api_error_code)
							{
								printf("NG\r\n");
								printf("R_FLASH_Erase() returns error code = %d.\r\n", flash_error_code);
								secure_boot_state = BOOT_LOADER_STATE_FATAL_ERROR;
								secure_boot_error_code = BOOT_LOADER_FAIL;
								break;
							}
							secure_boot_state = BOOT_LOADER_STATE_BANK0_INSTALL_SECURE_BOOT_ERASE_WAIT;
							break;

    	        		case BOOT_LOADER_STATE_BANK0_INSTALL_SECURE_BOOT_ERASE_WAIT:
    	            		/* this state will be update by flash callback */
    	        			break;

    	        		case BOOT_LOADER_STATE_BANK0_INSTALL_SECURE_BOOT_ERASE_COMPLETE:
    	        	        if (FLASH_SUCCESS == flash_error_code)
    	        	        {
    	        	            printf("OK\r\n");
    	        	        }
    	        	        else
    	        	        {
    	        	            printf("R_FLASH_Write() callback error. %d.\r\n", flash_error_code);
    	        	            printf("system error.\r\n");
    	        				secure_boot_state = BOOT_LOADER_STATE_FATAL_ERROR;
    	        				secure_boot_error_code = BOOT_LOADER_FAIL;
    	        	            break;
    	        	        }
    	        	        printf("copy secure boot (part1) from bank0 to bank1...");
    	        	        flash_api_error_code = R_FLASH_Write((uint32_t)BOOT_LOADER_LOW_ADDRESS, (uint32_t)BOOT_LOADER_MIRROR_LOW_ADDRESS, ((uint32_t)BOOT_LOADER_MIRROR_BLOCK_NUM_FOR_MEDIUM) * FLASH_CF_MEDIUM_BLOCK_SIZE);
							if(FLASH_SUCCESS != flash_api_error_code)
							{
								printf("NG\r\n");
								printf("R_FLASH_Erase() returns error code = %d.\r\n", flash_error_code);
								secure_boot_state = BOOT_LOADER_STATE_FATAL_ERROR;
								secure_boot_error_code = BOOT_LOADER_FAIL;
								break;
							}
							secure_boot_state = BOOT_LOADER_STATE_BANK0_INSTALL_SECURE_BOOT_WRITE_WAIT1;
							break;

    	        		case BOOT_LOADER_STATE_BANK0_INSTALL_SECURE_BOOT_WRITE_WAIT1:
    	            		/* this state will be update by flash callback */
    	        			break;

    	        		case BOOT_LOADER_STATE_BANK0_INSTALL_SECURE_BOOT_WRITE_COMPLETE1:
    	        	        if (FLASH_SUCCESS == flash_error_code)
    	        	        {
    	        	            printf("OK\r\n");
    	        	        }
    	        	        else
    	        	        {
    	        	            printf("R_FLASH_Write() callback error. %d.\r\n", flash_error_code);
    	        	            printf("system error.\r\n");
    	        				secure_boot_state = BOOT_LOADER_STATE_FATAL_ERROR;
    	        				secure_boot_error_code = BOOT_LOADER_FAIL;
    	        	            break;
    	        	        }
    	        	        printf("copy secure boot (part2) from bank0 to bank1...");
							if(BOOT_LOADER_MIRROR_BLOCK_NUM_FOR_MEDIUM > 0)
							{
								flash_api_error_code = R_FLASH_Write((uint32_t)FLASH_CF_BLOCK_7, (uint32_t)FLASH_CF_BLOCK_45, 8 * FLASH_CF_SMALL_BLOCK_SIZE);
								if(FLASH_SUCCESS != flash_api_error_code)
								{
									printf("NG\r\n");
									printf("R_FLASH_Write() returns error code = %d.\r\n", flash_error_code);
									secure_boot_error_code = BOOT_LOADER_FAIL;
									break;
								}
							}
							else
							{
								secure_boot_state = BOOT_LOADER_STATE_BANK0_INSTALL_SECURE_BOOT_WRITE_COMPLETE2;
							}
							secure_boot_state = BOOT_LOADER_STATE_BANK0_INSTALL_SECURE_BOOT_WRITE_WAIT2;
							break;

    	        		case BOOT_LOADER_STATE_BANK0_INSTALL_SECURE_BOOT_WRITE_WAIT2:
    	            		/* this state will be update by flash callback */
    	        			break;

    	        		case BOOT_LOADER_STATE_BANK0_INSTALL_SECURE_BOOT_WRITE_COMPLETE2:
    	        	        if (FLASH_SUCCESS == flash_error_code)
    	        	        {
    	        	            printf("OK\r\n");
    	        	        }
    	        	        else
    	        	        {
    	        	            printf("R_FLASH_Write() callback error. %d.\r\n", flash_error_code);
    	        	            printf("system error.\r\n");
    	        				secure_boot_state = BOOT_LOADER_STATE_FATAL_ERROR;
    	        				secure_boot_error_code = BOOT_LOADER_FAIL;
    	        	            break;
    	        	        }
							printf("========== install user program phase ==========\r\n");
							printf("erase install area (data flash): ");
							flash_api_error_code = R_FLASH_Erase((flash_block_address_t)BOOT_LOADER_USER_CONST_DATA_LOW_ADDRESS, BOOT_LOADER_UPDATE_CONST_DATA_TARGET_BLOCK_NUMBER);
							if (FLASH_SUCCESS != flash_api_error_code)
							{
								printf("R_FLASH_Erase() returns error. %d.\r\n", flash_error_code);
								printf("system error.\r\n");
								secure_boot_error_code = BOOT_LOADER_FAIL;
								break;
							}
							secure_boot_state = BOOT_LOADER_STATE_BANK0_INSTALL_DATA_FLASH_ERASE_WAIT;
							break;

    	        		case BOOT_LOADER_STATE_BANK0_INSTALL_DATA_FLASH_ERASE_WAIT:
    	            		/* this state will be update by flash callback */
    	        			break;

    	        		case BOOT_LOADER_STATE_BANK0_INSTALL_DATA_FLASH_ERASE_COMPLETE:
    	        	        if (FLASH_SUCCESS == flash_error_code)
    	        	        {
    	        	            printf("OK\r\n");
    	        	        }
    	        	        else
    	        	        {
    	        	            printf("R_FLASH_Write() callback error. %d.\r\n", flash_error_code);
    	        	            printf("system error.\r\n");
    	        				secure_boot_state = BOOT_LOADER_STATE_FATAL_ERROR;
    	        				secure_boot_error_code = BOOT_LOADER_FAIL;
    	        	            break;
    	        	        }
							printf("erase install area (code flash): ");
							flash_api_error_code = R_FLASH_Erase((flash_block_address_t)BOOT_LOADER_UPDATE_TEMPORARY_AREA_HIGH_ADDRESS, FLASH_NUM_BLOCKS_CF - BOOT_LOADER_MIRROR_BLOCK_NUM_FOR_SMALL - BOOT_LOADER_MIRROR_BLOCK_NUM_FOR_MEDIUM);
							if (FLASH_SUCCESS != flash_api_error_code)
							{
								printf("R_FLASH_Erase() returns error. %d.\r\n", flash_error_code);
								printf("system error.\r\n");
								secure_boot_error_code = BOOT_LOADER_FAIL;
								break;
							}
							secure_boot_state = BOOT_LOADER_STATE_BANK0_INSTALL_CODE_FLASH_ERASE_WAIT;
							break;

    	        		case BOOT_LOADER_STATE_BANK0_INSTALL_CODE_FLASH_ERASE_WAIT:
    	            		/* this state will be update by flash callback */
    	        			break;

    	        		case BOOT_LOADER_STATE_BANK0_INSTALL_CODE_FLASH_ERASE_COMPLETE:
    	        	        if (FLASH_SUCCESS == flash_error_code)
    	        	        {
    	        	            printf("OK\r\n");
    	        	        }
    	        	        else
    	        	        {
    	        	            printf("R_FLASH_Write() callback error. %d.\r\n", flash_error_code);
    	        	            printf("system error.\r\n");
    	        				secure_boot_state = BOOT_LOADER_STATE_FATAL_ERROR;
    	        				secure_boot_error_code = BOOT_LOADER_FAIL;
    	        	            break;
    	        	        }
    	        	        printf("send \"%s\" via UART.\r\n", INITIAL_FIRMWARE_FILE_NAME);
							secure_boot_state = BOOT_LOADER_STATE_BANK0_INSTALL_CODE_FLASH_READ_WAIT;
    	        	        break;

    	        		case BOOT_LOADER_STATE_BANK0_INSTALL_CODE_FLASH_READ_WAIT:
							/* install code flash area */
							if(!firm_block_read(load_firmware_control_block.flash_buffer, load_firmware_control_block.offset))
							{
								flash_api_error_code = R_FLASH_Write((uint32_t)load_firmware_control_block.flash_buffer, (uint32_t)BOOT_LOADER_UPDATE_TEMPORARY_AREA_LOW_ADDRESS + load_firmware_control_block.offset, sizeof(load_firmware_control_block.flash_buffer));
								if (FLASH_SUCCESS != flash_api_error_code)
								{
									printf("R_FLASH_Erase() returns error. %d.\r\n", flash_error_code);
									printf("system error.\r\n");
									secure_boot_error_code = BOOT_LOADER_FAIL;
									break;
								}
								secure_boot_state = BOOT_LOADER_STATE_BANK0_INSTALL_CODE_FLASH_WRITE_WAIT;
							}
							break;

    	        		case BOOT_LOADER_STATE_BANK0_INSTALL_CODE_FLASH_WRITE_WAIT:
    	            		/* this state will be update by flash callback */
    	        			break;

    	        		case BOOT_LOADER_STATE_BANK0_INSTALL_CODE_FLASH_WRITE_COMPLETE:
    	        	        if (FLASH_SUCCESS != flash_error_code)
    	        	        {
    	        	            printf("R_FLASH_Write() callback error. %d.\r\n", flash_error_code);
    	        	            printf("system error.\r\n");
    	        				secure_boot_state = BOOT_LOADER_STATE_FATAL_ERROR;
    	        				secure_boot_error_code = BOOT_LOADER_FAIL;
    	        	            break;
    	        	        }

							if(load_firmware_control_block.offset < (FLASH_CF_MEDIUM_BLOCK_SIZE * BOOT_LOADER_UPDATE_TARGET_BLOCK_NUMBER))
							{
								/* one more loop */
								secure_boot_state = BOOT_LOADER_STATE_BANK0_INSTALL_CODE_FLASH_READ_WAIT;
							}
							else if(load_firmware_control_block.offset == (FLASH_CF_MEDIUM_BLOCK_SIZE * BOOT_LOADER_UPDATE_TARGET_BLOCK_NUMBER))
							{
								printf("\n");
								printf("completed installing firmware.\r\n");
								printf("code flash hash check...");
								R_Sha1((uint8_t*)BOOT_LOADER_UPDATE_TEMPORARY_AREA_LOW_ADDRESS + BOOT_LOADER_USER_FIRMWARE_HEADER_LENGTH,
										hash_sha1, (FLASH_CF_MEDIUM_BLOCK_SIZE * BOOT_LOADER_UPDATE_TARGET_BLOCK_NUMBER) - BOOT_LOADER_USER_FIRMWARE_HEADER_LENGTH);
								if(0 == memcmp(hash_sha1, firmware_update_control_block_bank1->signature, SHA1_HASH_LENGTH_BYTE_SIZE))
								{
									printf("OK\r\n");
									secure_boot_state = BOOT_LOADER_STATE_BANK0_INSTALL_DATA_FLASH_WRITE_WAIT;
								}
								else
								{
									printf("NG\r\n");
	    	        				secure_boot_state = BOOT_LOADER_STATE_FATAL_ERROR;
	    	        				secure_boot_error_code = BOOT_LOADER_FAIL;
								}
							}
							else
							{
								printf("\n");
								printf("fatal error occurred.\r\n");
    	        				secure_boot_state = BOOT_LOADER_STATE_FATAL_ERROR;
    	        				secure_boot_error_code = BOOT_LOADER_FAIL;
							}
							load_firmware_control_block.offset += FLASH_CF_MEDIUM_BLOCK_SIZE;
							load_firmware_control_block.progress = (uint32_t)(((float)(load_firmware_control_block.offset)/(float)((FLASH_CF_MEDIUM_BLOCK_SIZE * BOOT_LOADER_UPDATE_TARGET_BLOCK_NUMBER))*100));
							printf("installing firmware...%d%(%d/%dKB).\r", load_firmware_control_block.progress, load_firmware_control_block.offset/1024, (FLASH_CF_MEDIUM_BLOCK_SIZE * BOOT_LOADER_UPDATE_TARGET_BLOCK_NUMBER)/1024);
							break;

							/* install data flash area */
							while(1)
							{
								if(!const_data_block_read(load_const_data_control_block.flash_buffer, load_const_data_control_block.offset))
								{
									flash_api_error_code = R_FLASH_Write((uint32_t)load_const_data_control_block.flash_buffer, (uint32_t)BOOT_LOADER_USER_CONST_DATA_LOW_ADDRESS + load_const_data_control_block.offset, sizeof(load_const_data_control_block.flash_buffer));
									if (FLASH_SUCCESS != flash_api_error_code)
									{
										printf("R_FLASH_Write() returns error. %d.\r\n", flash_error_code);
										printf("system error.\r\n");
										while(1);
									}
									load_const_data_control_block.offset += FLASH_DF_BLOCK_SIZE;
									load_const_data_control_block.progress = (uint32_t)(((float)(load_const_data_control_block.offset)/(float)((FLASH_DF_BLOCK_SIZE * BOOT_LOADER_UPDATE_CONST_DATA_TARGET_BLOCK_NUMBER))*100));
									static uint32_t previous_offset = 0;
									if(previous_offset != (load_const_data_control_block.offset/1024))
									{
										printf("installing const data...%d%(%d/%dKB).\r", load_const_data_control_block.progress, load_const_data_control_block.offset/1024, (FLASH_DF_BLOCK_SIZE * BOOT_LOADER_UPDATE_CONST_DATA_TARGET_BLOCK_NUMBER)/1024);
										previous_offset = load_const_data_control_block.offset/1024;
									}
									if(load_const_data_control_block.offset < (FLASH_DF_BLOCK_SIZE * BOOT_LOADER_UPDATE_CONST_DATA_TARGET_BLOCK_NUMBER))
									{
										/* one more loop */
									}
									else if(load_const_data_control_block.offset == (FLASH_DF_BLOCK_SIZE * BOOT_LOADER_UPDATE_CONST_DATA_TARGET_BLOCK_NUMBER))
									{
										printf("\n");
										printf("completed installing const data.\r\n");
										printf("software reset...\r\n");
										R_BSP_SoftwareDelay(3000, BSP_DELAY_MILLISECS);
										software_reset();
									}
									else
									{
										printf("\n");
										printf("fatal error occurred.\r\n");
										while(1);
									}
								}
							}

						}
					}
    				break;
    	        case LIFECYCLE_STATE_TESTING:
    	            printf("illegal status\r\n");
    	            printf("swap bank...");
    	            R_BSP_SoftwareDelay(3000, BSP_DELAY_MILLISECS);
    	            bank_swap_with_software_reset();
    	            while(1);
    	            break;
    	        case LIFECYCLE_STATE_VALID:
    	            printf("bank0(execute area) on code flash hash check...");
    	            R_Sha1((uint8_t*)BOOT_LOADER_UPDATE_EXECUTE_AREA_LOW_ADDRESS + BOOT_LOADER_USER_FIRMWARE_HEADER_LENGTH,
    	            		hash_sha1, (FLASH_CF_MEDIUM_BLOCK_SIZE * BOOT_LOADER_UPDATE_TARGET_BLOCK_NUMBER) - BOOT_LOADER_USER_FIRMWARE_HEADER_LENGTH);
    	            if(!memcmp(firmware_update_control_block_bank0->signature, hash_sha1, sizeof(hash_sha1)))
    	            {
    	                printf("OK\r\n");
    	            	if(firmware_update_control_block_bank1->image_flag != LIFECYCLE_STATE_BLANK)
    	            	{
    	                    printf("erase install area (code flash): ");
    	                    flash_api_error_code = R_FLASH_Erase((flash_block_address_t)BOOT_LOADER_UPDATE_TEMPORARY_AREA_HIGH_ADDRESS, FLASH_NUM_BLOCKS_CF - BOOT_LOADER_MIRROR_BLOCK_NUM_FOR_SMALL - BOOT_LOADER_MIRROR_BLOCK_NUM_FOR_MEDIUM);
    	                    if (FLASH_SUCCESS == flash_api_error_code)
    	                    {
    	                        printf("OK\r\n");
    	                    }
    	                    else
    	                    {
    	                        printf("R_FLASH_Erase() returns error. %d.\r\n", flash_error_code);
    	                        printf("system error.\r\n");
    	                        while(1);
    	                    }
    	            	}
    	                printf("jump to user program\r\n");
    	                R_BSP_SoftwareDelay(1000, BSP_DELAY_MILLISECS);
    	                secure_boot_error_code = BOOT_LOADER_SUCCESS;
    	            }
    	            else
    	            {
    					printf("NG.\r\n");
    					printf("Code flash is completely broken.\r\n");
    					printf("Please erase all code flash.\r\n");
    					printf("And, write secure boot using debugger.\r\n");
    					secure_boot_error_code = BOOT_LOADER_FAIL;
    	            }
    	            break;
    	        default:
    	            printf("illegal flash rom status code 0x%x.\r\n", firmware_update_control_block_bank0->image_flag);
    	            printf("bank1(temporary area) on code flash hash check...");
    	            R_Sha1((uint8_t*)BOOT_LOADER_UPDATE_TEMPORARY_AREA_LOW_ADDRESS + BOOT_LOADER_USER_FIRMWARE_HEADER_LENGTH,
    	            		hash_sha1, (FLASH_CF_MEDIUM_BLOCK_SIZE * BOOT_LOADER_UPDATE_TARGET_BLOCK_NUMBER) - BOOT_LOADER_USER_FIRMWARE_HEADER_LENGTH);
    	            if(!memcmp(firmware_update_control_block_bank1->signature, hash_sha1, sizeof(hash_sha1)))
    	            {
    	                printf("OK\r\n");
    	                R_BSP_SoftwareDelay(1000, BSP_DELAY_MILLISECS);
    	                bank_swap_with_software_reset();
    	            }
    	            else
    	            {
    	                printf("NG\r\n");
    	                R_BSP_SoftwareDelay(1000, BSP_DELAY_MILLISECS);
    	                software_reset();
    	            }
    }
    return secure_boot_error_code;
}

static void software_reset(void)
{
	/* stop all interrupt completely */
    set_psw(0);
    R_BSP_InterruptsDisable();
    R_BSP_RegisterProtectDisable(BSP_REG_PROTECT_LPC_CGC_SWR);
    SYSTEM.SWRR = 0xa501;
    while(1);   /* software reset */
}

static void bank_swap_with_software_reset(void)
{
	/* stop all interrupt completely */
    set_psw(0);
    R_BSP_InterruptsDisable();
    R_FLASH_Control(FLASH_CMD_BANK_TOGGLE, NULL);
    R_BSP_RegisterProtectDisable(BSP_REG_PROTECT_LPC_CGC_SWR);
    SYSTEM.SWRR = 0xa501;
    while(1);   /* software reset */
}


/***********************************************************************************************************************
* Function Name: firm_block_read
* Description  :
* Arguments    :
* Return Value :
***********************************************************************************************************************/
static int32_t firm_block_read(uint32_t *firmware, uint32_t offset)
{
	int32_t error_code = -1;
	if(offset == sci_receive_control_block.total_byte_size)
	{
		sci_receive_control_block.buffer_occupied_byte_size = 0;
		while(sci_receive_control_block.buffer_occupied_byte_size != FLASH_CF_MEDIUM_BLOCK_SIZE);
		memcpy(firmware, sci_receive_control_block.buffer, FLASH_CF_MEDIUM_BLOCK_SIZE);
		sci_receive_control_block.buffer_occupied_byte_size = 0;
		sci_receive_control_block.total_byte_size += FLASH_CF_MEDIUM_BLOCK_SIZE;
		error_code = 0;
	}
	else
	{
		error_code = -1;
	}
	return error_code;
}

/***********************************************************************************************************************
* Function Name: const_data_block_read
* Description  :
* Arguments    :
* Return Value :
***********************************************************************************************************************/
static int32_t const_data_block_read(uint32_t *const_data, uint32_t offset)
{

}

/*****************************************************************************
* Function Name: my_sci_callback
* Description  : This is a template for an SCI Async Mode callback function.
* Arguments    : pArgs -
*                pointer to sci_cb_p_args_t structure cast to a void. Structure
*                contains event and associated data.
* Return Value : none
******************************************************************************/
uint32_t error_count1 = 0;
uint32_t error_count2 = 0;
uint32_t rcv_count1 = 0;
uint32_t rcv_count2 = 0;

static void my_sci_callback(void *pArgs)
{
    sci_cb_args_t   *p_args;

    p_args = (sci_cb_args_t *)pArgs;

    if (SCI_EVT_RX_CHAR == p_args->event)
    {
        /* From RXI interrupt; received character data is in p_args->byte */
    	if(sci_receive_control_block.buffer_occupied_byte_size < sizeof(sci_receive_control_block.buffer))
    	{
    		R_SCI_Receive(p_args->hdl, &sci_receive_control_block.buffer[sci_receive_control_block.buffer_occupied_byte_size++], 1);
        	rcv_count1++;
    	}
    	rcv_count2++;
    }
    else if (SCI_EVT_RXBUF_OVFL == p_args->event)
    {
        /* From RXI interrupt; rx queue is full; 'lost' data is in p_args->byte
           You will need to increase buffer size or reduce baud rate */
    	nop();
    	error_count1++;
    }
    else if (SCI_EVT_OVFL_ERR == p_args->event)
    {
        /* From receiver overflow error interrupt; error data is in p_args->byte
           Error condition is cleared in calling interrupt routine */
    	nop();
    	error_count2++;
    }
    else if (SCI_EVT_FRAMING_ERR == p_args->event)
    {
        /* From receiver framing error interrupt; error data is in p_args->byte
           Error condition is cleared in calling interrupt routine */
    	nop();
    }
    else if (SCI_EVT_PARITY_ERR == p_args->event)
    {
        /* From receiver parity error interrupt; error data is in p_args->byte
           Error condition is cleared in calling interrupt routine */
    	nop();
    }
    else
    {
        /* Do nothing */
    }

} /* End of function my_sci_callback() */

/***********************************************************************************************************************
* Function Name: my_flash_callback
* Description  :
* Arguments    :
* Return Value :
***********************************************************************************************************************/
static void my_flash_callback(void *event)
{
	uint32_t event_code = FLASH_ERR_FAILURE;
	event_code = *((uint32_t*)event);

    if((event_code == FLASH_INT_EVENT_WRITE_COMPLETE) || (event_code == FLASH_INT_EVENT_ERASE_COMPLETE))
    {
    	flash_error_code = FLASH_SUCCESS;
    }

	switch(secure_boot_state)
	{
		case BOOT_LOADER_STATE_BANK1_UPDATE_LIFECYCLE_ERASE_WAIT:
			secure_boot_state = BOOT_LOADER_STATE_BANK1_UPDATE_LIFECYCLE_ERASE_COMPLETE;
			break;
		case BOOT_LOADER_STATE_BANK1_UPDATE_LIFECYCLE_WRITE_WAIT:
			secure_boot_state = BOOT_LOADER_STATE_BANK1_UPDATE_LIFECYCLE_WRITE_COMPLETE;
			break;
		case BOOT_LOADER_STATE_BANK0_INSTALL_SECURE_BOOT_ERASE_WAIT:
			secure_boot_state = BOOT_LOADER_STATE_BANK0_INSTALL_SECURE_BOOT_ERASE_COMPLETE;
			break;
		case BOOT_LOADER_STATE_BANK0_INSTALL_SECURE_BOOT_WRITE_WAIT1:
			secure_boot_state = BOOT_LOADER_STATE_BANK0_INSTALL_SECURE_BOOT_WRITE_COMPLETE1;
			break;
		case BOOT_LOADER_STATE_BANK0_INSTALL_SECURE_BOOT_WRITE_WAIT2:
			secure_boot_state = BOOT_LOADER_STATE_BANK0_INSTALL_SECURE_BOOT_WRITE_COMPLETE2;
			break;
		case BOOT_LOADER_STATE_BANK0_INSTALL_DATA_FLASH_ERASE_WAIT:
			secure_boot_state = BOOT_LOADER_STATE_BANK0_INSTALL_DATA_FLASH_ERASE_COMPLETE;
			break;
		case BOOT_LOADER_STATE_BANK0_INSTALL_CODE_FLASH_ERASE_WAIT:
			secure_boot_state = BOOT_LOADER_STATE_BANK0_INSTALL_CODE_FLASH_ERASE_COMPLETE;
			break;
		case BOOT_LOADER_STATE_BANK0_INSTALL_CODE_FLASH_WRITE_WAIT:
			secure_boot_state = BOOT_LOADER_STATE_BANK0_INSTALL_CODE_FLASH_WRITE_COMPLETE;
			break;
		case BOOT_LOADER_STATE_BANK0_INSTALL_DATA_FLASH_WRITE_WAIT:
			secure_boot_state = BOOT_LOADER_STATE_BANK0_INSTALL_CODE_FLASH_WRITE_COMPLETE;
			break;
		default:
			break;
	}
}

/***********************************************************************************************************************
 * Function Name: my_sw_charput_function
 * Description  : char data output API
 * Arguments    : data -
 *                    Send data with SCI
 * Return Value : none
 **********************************************************************************************************************/
void my_sw_charput_function(uint8_t data)
{
    uint32_t arg = 0;
    /* do not call printf()->charput in interrupt context */
    do
    {
        /* Casting void pointer is used for address. */
        R_SCI_Control(my_sci_handle, SCI_CMD_TX_Q_BYTES_FREE, (void*)&arg);
    }
    while (SCI_CFG_CH8_TX_BUFSIZ != arg);
    /* Casting uint8_t pointer is used for address. */
    R_SCI_Send(my_sci_handle, (uint8_t*)&data, 1);

    return;
}
/***********************************************************************************************************************
 End of function my_sw_charput_function
 **********************************************************************************************************************/

void my_sw_charget_function(void)
{

}

static const uint8_t *get_status_string(uint8_t status)
{
	static const uint8_t status_string[][32] = {{"LIFECYCLE_STATE_BLANK"},{"LIFECYCLE_STATE_TESTING"},{"LIFECYCLE_STATE_VALID"},{"LIFECYCLE_STATE_INVALID"},{"LIFECYCLE_STATE_UNKNOWN"}};
	const uint8_t *tmp;

	if(status == LIFECYCLE_STATE_BLANK)
	{
		tmp = status_string[0];
	}
	else if(status == LIFECYCLE_STATE_TESTING)
	{
		tmp = status_string[1];
	}
	else if(status == LIFECYCLE_STATE_VALID)
	{
		tmp = status_string[2];
	}
	else if(status == LIFECYCLE_STATE_INVALID)
	{
		tmp = status_string[3];
	}
	else
	{
		tmp = status_string[4];
	}
	return tmp;
}
