#include <string.h>
#include <assert.h>
#include <stdbool.h>
#include <time.h>
 
#include <event.h>

#include "paxos_config.h"
#include "lp_utils.h"
#include "lp_topology.h"
#include "lp_timers.h"
#include "lp_clients_handling.h"
#include "lp_config_parser.h"
#include "lp_network.h"
#include "lp_delivery_repeat.h"
#include "lp_stable_storage.h"
#include "lp_mcaster_storage.h"
#include "lp_submit_proxy.h"

#include "ringpaxos_messages.h"

typedef struct acceptor_t {
	
	config_mngr * cfg;
	clival_mngr * cvm;
	topolo_mngr * tm;
		
	udp_receiver * pred_recv;
	udp_sender * succ_send;
	periodic_event * print_counters_ev;
	struct timeval print_counters_interval;

//Only multicaster
	udp_sender * mcast_send;
	periodic_event * periodic_ev;
	mcaster_storage_mngr * msm;
	struct timeval mcaster_clock; 

	unsigned p1_ready_count;
	unsigned p1_pending_count;
	iid_t p1_highest_open;

	//Highest instance such that all lower ones are closed.
	// I.e. 1:Cl, 2:Op, 3:Op, 4:Cl
	// highest_closed is 1, NOT 4
	iid_t highest_closed_iid;
	//Highest instance for which some action was taken 
	// (meaning that phase 2 was executed at least once).
	iid_t highest_open_iid;

	struct mcaster_event_counters {
		long unsigned p1_timeout;
		long unsigned p2_timeout;
		long unsigned p2_waits_p1;
		long unsigned out_of_values;
		long unsigned p2_window_full;
		long unsigned p1_range_try;
		long unsigned p1_range_success;
		long unsigned map_request;
		long unsigned map_request_ignored;
		long unsigned chosenval_request;
		long unsigned dropped_client_values;
		int last_print_time;
		//TODO if lpconfig_get_max_p2_open_per_iteration(acc->cfg) is >= 100 will crash
		unsigned concurrent_p2_open[100]; 
	} mec;
	
// Only non-multicaster
	stable_storage_mngr * ssm;
	udp_receiver * mcast_recv;
	learner_mngr * lm;

	iid_t highest_instance_seen;

	struct acceptor_event_counters {
		long unsigned range_promise;
		long unsigned range_refuse;
		long unsigned p2_noval_refuse;
	} aec;
} acceptor;


bool am_i_leader(acceptor * acc) {
    //TODO low_priority
    return (lpconfig_get_self_acceptor_id(acc->cfg) == 1);
}

bool am_i_first_in_ring(acceptor * acc) {
    //TODO low_priority
    return (lpconfig_get_self_acceptor_id(acc->cfg) == 2);	
}

//Acceptor events are defined in a separate file
#include "acceptor_helpers.c"
#include "acceptor_helpers_mcaster.c"
#include "acceptor_events.c"

void common_init(acceptor * acc) {
	
	int result = 0;
		
	// "clean" exit when a SIGINT (ctrl-c) is received
	enable_ctrl_c_handler();
    
	//Initialize libevent
    event_init();
    
    // Set event for topology change
    result = topology_mngr_init(
        on_topology_change,   /*Callback for ring update*/
		acc,
		acc->cfg,
		&acc->tm
    );
	assert(result == 0);
    LOG_MSG(DEBUG, ("Topology manager initialized!\n"));
    
    //Open UDP connection to successor
    acc->succ_send = udp_sender_init(
        lptopo_get_successor_addr(acc->tm),  /*Ring addr of successor*/
        lptopo_get_successor_port(acc->tm),   /*Ring port for successor*/
		acc->cfg
        );
    assert(acc->succ_send != NULL);
	udp_sender_enable_autoflush(acc->succ_send, 1);
    LOG_MSG(DEBUG, ("Successor sender intialized (%s:%d)!\n", 
        lptopo_get_successor_addr(acc->tm), 
        lptopo_get_successor_port(acc->tm)));


    // Open listen port for predecessor
    // Set event for predecessor messages
    acc->pred_recv = udp_receiver_init(
        lpconfig_get_ring_inbound_addr(acc->cfg),        /*Ring addr address*/
        lpconfig_get_ring_inbound_port(acc->cfg), /*Ring port for predecessor*/
        on_predecessor_msg, /*Message from prede is received*/
		acc,
		acc->cfg
        );
    assert(acc->pred_recv != NULL);
    

    LOG_MSG(DEBUG, ("Predecessor receiver intialized (%s:%d)!\n", 
        lpconfig_get_ring_inbound_addr(acc->cfg), 
        lpconfig_get_ring_inbound_port(acc->cfg)));
}

void multicaster_init(acceptor * acc) {
	bool success;
	
    LOG_MSG(PAXOS, ("This acceptor (id:%d) is the current leader\n", 
        lpconfig_get_self_acceptor_id(acc->cfg)));

    // Open listen socket for clients
    // Set event for client values submission
    //Create an in-memory structure that stores paxos commands (a.k.a. client values)
    success = client_values_mngr_UDP_init(
        mcaster_handle_value_submit, /*Called when a client sends a value*/
		acc,
		&acc->cvm,
		acc->cfg
        );
    assert(success);
	success = client_values_mngr_init_queue(acc->cvm, 250); //TODO make config parameter
    LOG_MSG(DEBUG, ("Client values manager initialized!\n"));
    
    acc->msm = mcaster_storage_init(acc->cfg, acc->cvm);
    assert(acc->msm != NULL);
    LOG_MSG(DEBUG, ("Multicast sender initialized"))

    //Create UDP multicast socket manager
    acc->mcast_send = mcast_sender_init(
        lpconfig_get_mcast_addr(acc->cfg), /*Mcast addr */
        lpconfig_get_mcast_port(acc->cfg), /*Mcast port*/
		acc->cfg
        );
	//No autoflush, flush happens in mcaster_flush_send_buffers
    assert(acc->mcast_send != NULL);
    LOG_MSG(DEBUG, ("Multicast sender initialized (%s:%d)!\n", 
        lpconfig_get_mcast_addr(acc->cfg), 
        lpconfig_get_mcast_port(acc->cfg)));

    // Set periodic event for various routine checks
    acc->periodic_ev = set_periodic_event(
        lpconfig_get_mcaster_clock_interval(acc->cfg),  /*Interval for this event*/
        on_mcaster_periodic_check, /*Called periodically to execute P2*/
        acc /*Argument passed to above function*/
        );
    assert(acc->periodic_ev != NULL);

	//Update clock when receiving predecessor/learner messages                     
	udp_receiver_set_preread_callback(acc->pred_recv, mcaster_update_wallclock);

	//Init event counters
	if(LP_EVENTCOUNTERS != LOG_NONE) {
		memset(&acc->mec, '\0', sizeof(struct mcaster_event_counters));
		acc->print_counters_ev = set_periodic_event(
			&acc->print_counters_interval,  /*Interval for this event*/
			mcaster_print_counters, /*Called periodically to execute P2*/
			acc /*Argument passed to above function*/
			);
		assert(acc->print_counters_ev != NULL);
	}

}

void regular_acceptor_init(acceptor * acc) {
    LOG_MSG(PAXOS, ("This acceptor (id:%d) is NOT the current leader\n",
    lpconfig_get_self_acceptor_id(acc->cfg)));
    
    // Init storage: a structure indexed by instance ID
    // that abstract an infinite array
    acc->ssm = stable_storage_init(acc->cfg);
    assert(acc->ssm != NULL);
    LOG_MSG(DEBUG, ("Stable storage manager initialized!\n"));

    // Open multicast listen socket, 
    // Set event for multicast messages
    acc->mcast_recv = mcast_receiver_init(
        lpconfig_get_mcast_addr(acc->cfg),              /*Multicast address*/
        lpconfig_get_mcast_port(acc->cfg),                 /*Multicast port*/
        on_multicast_msg, /*Called when a message is received*/
		acc, 
		acc->cfg
        );
    assert(acc->mcast_recv != NULL);
	//Set callback invoked after processing a batch of messages
	//udp_receiver_set_postdeliver_callback(acc->mcast_recv, acceptor_flush_successor_socket, acc);
    LOG_MSG(DEBUG, ("Multicast receiver intialized (%s:%d)!\n", 
        lpconfig_get_mcast_addr(acc->cfg), 
        lpconfig_get_mcast_port(acc->cfg)));


	// TODO useless at the moment, remove!
    // Open listen port for learners
    // Set event for learners requests
    acc->lm = learner_mngr_TCP_init(
        lpconfig_get_learners_inbound_addr(acc->cfg), /*Addr for accepting learners connections*/
        lpconfig_get_learners_inbound_port(acc->cfg), /*Port for accepting learners connections*/
        on_repeat_request /*Called when a learner requires a repeat*/
        );
    assert(acc->lm != NULL);
    LOG_MSG(DEBUG, ("Learners manager initialized (%s:%d)!\n", 
        lpconfig_get_learners_inbound_addr(acc->cfg), 
        lpconfig_get_learners_inbound_port(acc->cfg)));

	//Set callback invoked after processing a batch of messages
	//udp_receiver_set_postdeliver_callback(acc->pred_recv, acceptor_flush_successor_socket, acc);

	//Init event counters
	if(LP_EVENTCOUNTERS != LOG_NONE) {
		memset(&acc->aec, '\0', sizeof(struct acceptor_event_counters));
		acc->print_counters_ev = set_periodic_event(
			&acc->print_counters_interval,  /*Interval for this event*/
			acceptor_print_counters, /*Called periodically to execute P2*/
			acc /*Argument passed to above function*/
			);
		assert(acc->print_counters_ev != NULL);
	}
	
}

int acceptor_init(const char * config_file_path, acceptor_id_t acc_id, acceptor ** acc_ptr) {

    int result;

	acceptor * acc = malloc(sizeof(acceptor));
	assert(acc != NULL);
	memset(acc, '\0', sizeof(acceptor));
	
	LP_COUNTERS_PRINT_INTERVAL(acc->print_counters_interval);
	
    // Parse configuration file
    result = config_mngr_init(
        config_file_path, /*Path to config file*/
        acc_id,           /*Numeric ID of this acceptor*/
        on_configuration_change, /*Callback for config update*/
		acc,
		&acc->cfg
        );
    assert(result == 0);
    LOG_MSG(DEBUG, ("Configuration manager initialized!\n"));    

    //Initialization in common for all acceptors
    common_init(acc);
    
    if(am_i_leader(acc)) {
        
        //This acceptor is coordinator/multicaster
        multicaster_init(acc);
        
    } else {
        
        //This acceptor NOT is coordinator/multicaster
        regular_acceptor_init(acc);
    }

	*acc_ptr = acc;

    LOG_MSG(INFO, ("Acceptor %d: initialization completed!\n", acc_id));

	return 0;
}

int main (int argc, char const *argv[]) {
    
    int result;
    const char * config_file_path;
    
    print_cmdline_args(argc, argv);
    
    validate_acceptor_args_or_quit(argc, argv);
    acceptor_id_t acc_id = atoi(argv[1]);
    config_file_path = argv[2];

	acceptor * acc;
	
	//Multiple acceptors lanunched from here are ok
	//(i.e. belonging to different rings)
	result = acceptor_init(config_file_path, acc_id, &acc);
    assert(result == 0);
    LOG_MSG(DEBUG, ("Acceptor initialized!\n"));

    //Enter libevent infinite loop
    event_dispatch();
    
    return 0;
}
