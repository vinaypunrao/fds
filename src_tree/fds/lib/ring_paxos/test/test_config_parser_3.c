#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "forced_assert.h"

#include "lp_utils.h"
#include "lp_config_parser.h"
#include "test_header.h"

int main (int argc, char const *argv[]) {

    UNUSED_ARG(argc);
    UNUSED_ARG(argv);
    
    int result = -1;
    acceptor_id_t acc_id;
    
    // Valid config
    acc_id = 2;

	config_mngr * cfg;
	
    result = config_mngr_init("./etc/config3.cfg", acc_id, NULL,  NULL, &cfg);

    assert(result == 0);

    assert(lpconfig_get_p1_interval(cfg)->tv_sec == 1);
    assert(lpconfig_get_p1_interval(cfg)->tv_usec == 100);
    assert(lpconfig_get_p2_interval(cfg)->tv_sec == 2);
    assert(lpconfig_get_p2_interval(cfg)->tv_usec == 200);

    assert(lpconfig_get_acceptors_count(cfg) == 3);
    
    assert(strcmp("11.22.33.44", lpconfig_get_ring_inbound_addr(cfg)) == 0);
    assert(lpconfig_get_ring_inbound_port(cfg) == 7772);
    assert(lpconfig_get_learners_inbound_port(cfg) == 5552);
    
    assert(strcmp("111.222.112.221", lpconfig_get_ip_addr_of(cfg, 3)) == 0);
    assert(lpconfig_get_ring_port_of(cfg, 3) == 7773);
    assert(lpconfig_get_learners_port_of(cfg, 3) == 5553);

	assert(strcmp("239.00.0.1", lpconfig_get_mcast_addr(cfg)) == 0);
	assert(lpconfig_get_mcast_port(cfg) == 6667); 
	
	assert(lpconfig_get_quorum_size(cfg) == 2);
	
	//Test that the following have the right default value
	assert(lpconfig_get_delivery_check_interval(cfg)->tv_sec == 1); 
	assert(lpconfig_get_delivery_check_interval(cfg)->tv_usec == 0); 
	
	assert(lpconfig_get_default_autoflush_interval(cfg) == 50); 
	assert(lpconfig_get_working_set_size(cfg) == 100); 
	assert(lpconfig_get_max_active_instances(cfg) == 25); 
	assert(lpconfig_get_preexecution_window_size(cfg) == 50); 
	assert(lpconfig_get_mcaster_clock_interval(cfg)->tv_sec == 0); 
	assert(lpconfig_get_mcaster_clock_interval(cfg)->tv_usec == 100000);
	
	
	assert(lpconfig_get_max_p2_open_per_iteration(cfg) == 10); 
	assert(lpconfig_get_max_client_values_queue_size(cfg) == 100);
	
	assert(lpconfig_get_retransmit_request_interval(cfg)->tv_sec == 1); 
	assert(lpconfig_get_retransmit_request_interval(cfg)->tv_usec == 0);
	// assert(lpconfig_get_socket_buffers_size(cfg) == 2097152); 
	
		
    lpconfig_destroy(cfg);
    
	printf("TEST SUCCESSFUL!\n");
    return 0;
}
