/*
 * Copyright 2020 International Business Machines
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <getopt.h>
#include <malloc.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <assert.h>

#include <osnap_tools.h>
#include <libosnap.h>
#include <action_udp.H>
#include <osnap_hls_if.h>
#include <inttypes.h>

#define NPACKETS 1

//int verbose_flag = 0;

//static const char *version = GIT_VERSION;

static const char *mem_tab[] = { "HOST_DRAM", "CARD_DRAM", "TYPE_NVME" };

// Function that fills the MMIO registers / data structure 
// these are all data exchanged between the application and the action
static void snap_prepare_rx100G(struct snap_job *cjob,
				 struct rx100G_job *mjob,                               
				 void *addr_out,
				 uint32_t size_out,
				 uint8_t type_out)
{
	fprintf(stderr, "  prepare rx100G job of %ld bytes size\n", sizeof(*mjob));

	assert(sizeof(*mjob) <= SNAP_JOBSIZE);
	memset(mjob, 0, sizeof(*mjob));

	// Setting output params : where result will be written in host memory
	snap_addr_set(&mjob->out_frame_buffer, addr_out, size_out, type_out,
		      SNAP_ADDRFLAG_ADDR | SNAP_ADDRFLAG_DST |
		      SNAP_ADDRFLAG_END);

	mjob->packets_to_read = NPACKETS;
	mjob->fpga_mac_addr = 0xAABBCCDDEEF1;
	mjob->fpga_ipv4_addr = 0x0A013205;
	snap_job_set(cjob, mjob, sizeof(*mjob), NULL, 0);
}

/**
 * @brief	prints valid command line options
 *
 * @param prog	current program's name
 */
static void usage(const char *prog)
{
	printf("Usage: %s -C, --card <cardno>       can be (0...3)\n", prog);
}

/* main program of the application for the hls_udp example        */
/* This application will always be run on CPU and will call either       */
/* a software action (CPU executed) or a hardware action (FPGA executed) */
/* ==> This code was extracted from hls_rx100G action written by PSI with their kind authorization <== */
int main(int argc, char *argv[])
{
	// Init of all the default values used 
	int ch, rc = 0;
	int card_no = 0;
	struct snap_card *card = NULL;
	struct snap_action *action = NULL;
	char device[128];
	struct snap_job cjob;
	struct rx100G_job mjob;
	unsigned long timeout = 600;
	struct timeval etime, stime;
	//ssize_t size = NPACKETS * 400 * 64;
	ssize_t size = NPACKETS * 130 * 64;
	uint8_t *obuff = NULL;
	uint8_t type_out = SNAP_ADDRTYPE_HOST_DRAM;

	uint64_t addr_out = 0x0ull;

	int exit_code = EXIT_SUCCESS;

	// default is interrupt mode enabled (vs polling)
	//snap_action_flag_t action_irq = (SNAP_ACTION_DONE_IRQ | SNAP_ATTACH_IRQ);
	snap_action_flag_t action_irq = SNAP_ACTION_DONE_IRQ;

	/* Allocate in host memory the place to put the text processed */
	obuff = snap_malloc(size); //64Bytes aligned malloc
	if (obuff == NULL) goto out_error;
	memset(obuff, 0x0, size);

	
	// collecting the command line arguments
	while (1) {
		int option_index = 0;
		static struct option long_options[] = {
			{ "card",	 required_argument, NULL, 'C' },
			{ "no-irq",	 no_argument,	    NULL, 'N' },
			{ "help",	 no_argument, NULL, 'h' },
		};

		ch = getopt_long(argc, argv, "C:Nh", long_options, &option_index);
		if (ch == -1)
			break;
		switch (ch) {
			case 'C':
				card_no = strtol(optarg, (char **)NULL, 0);
				break;
			case 'h':
				usage(argv[0]);
				exit(EXIT_SUCCESS);
				break;
			case 'N':
				action_irq = 0;
				break;
			default:
				usage(argv[0]);
				exit(EXIT_FAILURE);
		}
	}

	// prepare params to be written in MMIO registers for action
	type_out = SNAP_ADDRTYPE_HOST_DRAM;
	addr_out = (unsigned long)obuff;


	printf("HLS_UDP program\n");
	printf("     this program is an example of use of udp in oc-accel\n\n");

	/* Display the parameters that will be used for the example */
	printf("PARAMETERS:\n"
	       "  type_out:    %x %s\n"
	       "  addr_out:    %016llx\n"
	       "  size_out:    %08lx\n",
	       type_out, mem_tab[type_out], (long long)addr_out,
	       size);

	// Allocate the card that will be used
        if(card_no == 0)
                snprintf(device, sizeof(device)-1, "IBM,oc-snap");
        else
                snprintf(device, sizeof(device)-1, "/dev/ocxl/IBM,oc-snap.000%d:00:00.1.0", card_no);

	card = snap_card_alloc_dev(device, SNAP_VENDOR_ID_IBM,
				   SNAP_DEVICE_ID_SNAP);
	if (card == NULL) {
		fprintf(stderr, "err: failed to open card %u: %s\n",
			card_no, strerror(errno));
                fprintf(stderr, "\n==> Did you consider running this command using sudo? <==\n");
		goto out_error;
	}

	// Attach the action that will be used on the allocated card
	action = snap_attach_action(card, ACTION_TYPE, action_irq, 60);
	if(action_irq)
		snap_action_assign_irq(action, ACTION_IRQ_SRC_LO);
	if (action == NULL) {
		fprintf(stderr, "err: failed to attach action %u: %s\n",
			card_no, strerror(errno));
		goto out_error1;
	}
 
	// Fill the stucture of data exchanged with the action
	snap_prepare_rx100G(&cjob, &mjob,
			     (void *)addr_out, size, type_out);
	printf("  mac :    %08llx\n"
	       "  ip  :    %08llx\n",
		   (long long)mjob.fpga_mac_addr, (long long)mjob.fpga_ipv4_addr);



	// uncomment to dump the job structure
	printf("\n Dump of the job structure\n");
	__hexdump(stderr, &mjob, sizeof(mjob));


	// Collect the timestamp BEFORE the call of the action
	gettimeofday(&stime, NULL);

	// Call the action will:
	//    write all the registers to the action (MMIO) 
	//  + start the action 
	//  + wait for completion
	//  + read all the registers from the action (MMIO) 
	printf("\ncall snap_action\n");
	rc = snap_action_sync_execute_job(action, &cjob, timeout);

	// Collect the timestamp AFTER the call of the action
	gettimeofday(&etime, NULL);
	if (rc != 0) {
		fprintf(stderr, "err: job execution %d: %s!\n", rc,
			strerror(errno));
		goto out_error2;
	}
	printf("\nDump of the output buff ( onlythe first 512 bytes )\n");
    	__hexdump(stdout, obuff, 512);



	printf(" Good packets %ld\n", mjob.good_packets);
	printf(" Bad packets %ld\n", mjob.bad_packets);
	printf(" Ignored Packets %ld\n", mjob.ignored_packets);

	// test return code
	(cjob.retc == SNAP_RETC_SUCCESS) ? fprintf(stdout, "SUCCESS\n") : fprintf(stdout, "FAILED\n");
	if (cjob.retc != SNAP_RETC_SUCCESS) {
		fprintf(stderr, "err: Unexpected RETC=%x!\n", cjob.retc);
		goto out_error2;
	}

	// Display the time of the action call (MMIO registers filled + execution)
	fprintf(stdout, "HLS UDP took %lld usec\n",
		(long long)timediff_usec(&etime, &stime));

	// Detach action + disallocate the card
	snap_detach_action(action);
	snap_card_free(card);

	__free(obuff);
	exit(exit_code);

 out_error2:
	snap_detach_action(action);
 out_error1:
	snap_card_free(card);
 out_error:
	__free(obuff);
	exit(EXIT_FAILURE);
}
