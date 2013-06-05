#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <stdbool.h>

#include "paxos_config.h"
#include "lp_config_parser.h"
#include "lp_topology.h"
#include "lp_network.h"
#include "lp_utils.h"
#include "lp_submit_proxy.h"

#define LARGEST_PAXOS_HEADER 14 //Phase1 

struct submit_proxy_t {
	udp_sender * us;
	config_mngr * cfg;
	topolo_mngr * tm;
	bool initialized;
	char buf[MAX_MESSAGE_SIZE];
};

static void on_topology_change_proxy(topology_info * tinfo, void * arg) {
	// TODO low priority
	UNUSED_ARG(tinfo);
	
	submit_proxy * sp = arg;
	assert(sp->initialized);
	UNUSED_ARG(sp);
};

static void on_configuration_change_proxy(config_mngr * old_cfg, config_mngr * new_cfg, void * arg) {
	// TODO low priority
	UNUSED_ARG(old_cfg);
	UNUSED_ARG(new_cfg);

	submit_proxy * sp = arg;
	assert(sp->initialized);
	
	UNUSED_ARG(sp);
};

submit_proxy * submit_proxy_udp_init(const char * config_path) {
		
	submit_proxy * sp = calloc(1, sizeof(submit_proxy));
	assert(sp != NULL);
	assert(!sp->initialized);
	
	
	int result;
	
    // Parse configuration file
    result = config_mngr_init(
        config_path, /*Path to config file*/
        0,           /*Numeric ID of this acceptor*/
        on_configuration_change_proxy, /*Callback for config update*/
		sp,
		&sp->cfg
        );
    assert(result == 0);
    LOG_MSG(DEBUG, ("Configuration manager initialized!\n"));

    result = topology_mngr_init(
        on_topology_change_proxy,   /*Callback for ring update*/
		sp,
		sp->cfg, 
		&sp->tm
    );
	assert(result == 0);
	
	sp->us = udp_sender_init(
		lptopo_get_leader_addr(sp->tm), 
		lptopo_get_leader_clients_port(sp->tm), 
		sp->cfg);
	assert(sp->us != NULL);	

	sp->initialized = true;

	return sp;
}

void submit_command(submit_proxy * sp, submit_cmd_msg * scm) {
	assert(sp->initialized);
	
	if(SUBMIT_CMD_MSG_SIZE(scm) > (MAX_MESSAGE_SIZE - LARGEST_PAXOS_HEADER)) {
		printf("Submit message too big (%lu), dropped!\n", SUBMIT_CMD_MSG_SIZE(scm));
		return;
	}
	
	net_send_udp(sp->us, scm, SUBMIT_CMD_MSG_SIZE(scm), client_submit);
}

void submit_proxy_flush_now(submit_proxy * sp) {
	assert(sp->initialized);
	
	udp_sender_force_flush(sp->us);
}
