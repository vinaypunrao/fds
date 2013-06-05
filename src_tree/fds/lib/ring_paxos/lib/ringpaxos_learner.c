#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <time.h>

#include "lp_learner.h"
#include "lp_network.h"
#include "lp_config_parser.h"
#include "lp_utils.h"
#include "ringpaxos_messages.h"

struct learner_event_counters {
	long unsigned map_request;
	long unsigned chosenval_request;
};

struct learner_t {
	bool initialized;
	struct learner_event_counters lec;
	udp_sender * mcast_send;
	udp_receiver * mcast_recv;
	delivery_queue * dq;
	config_mngr * cfg;
	topolo_mngr * tm;
	char missing_cmd_request_buf[MAX_MESSAGE_SIZE];	
	char missing_acc_request_buf[MAX_MESSAGE_SIZE];
	void * cb_arg;
};


//TODO HACK to allow a learner to start later on without
//delivering all previous values;
void learner_delayed_start(learner_context * l) {
	assert(l->initialized);
	dq_delayed_start(l->dq);
}

void learner_print_eventcounters(learner_context * l) {
	assert(l->initialized);

	PRINT_COUNT(l->lec.map_request);
	PRINT_COUNT(l->lec.chosenval_request);
	printf("To multicaster:\n");
	udp_sender_print_stats(l->mcast_send, time(NULL));
	printf("From multicaster\n");
	udp_receiver_print_stats(l->mcast_recv, time(NULL));
}

void on_configuration_change(config_mngr * old, config_mngr * new, void * arg) {
    LOG_MSG(INFO, ("Configuration changed!\n"));
    // TODO low_priority
    UNUSED_ARG(old);
    UNUSED_ARG(new);

	learner_context * l = arg;
	assert(l->initialized);
	
	UNUSED_ARG(l);
}

void on_topology_change(topology_info * tm, void * arg) {
	UNUSED_ARG(tm);

	learner_context * l = arg;
	assert(l->initialized);
	
	UNUSED_ARG(l);
    LOG_MSG(INFO, ("Topology changed!\n"));
    
    // TODO low_priority
	//Re-connect to some acceptor for retransmissions
}

void on_missing_cmdmap(iid_t inst_number, void * arg) {
	learner_context * l = arg;
	assert(l->initialized);
	
	map_requests_msg * msg = (map_requests_msg*)&l->missing_cmd_request_buf;
	COUNT_EVENT(PAXOS, l->lec.map_request);
	msg->inst_number[msg->requests_count] = inst_number;
	msg->requests_count += 1;
	
	if(msg->requests_count >= REPEAT_REQUEST_MAX_ENTRIES) {
		net_send_udp(l->mcast_send, msg, CMDMAP_REQS_MSG_SIZE(msg), map_request);		
		udp_sender_force_flush(l->mcast_send);
		msg->requests_count = 0;
	}	
}

void on_missing_acceptance(iid_t inst_number, void * arg) {
	learner_context * l = arg;
	assert(l->initialized);
	
	chosencmd_requests_msg * msg = (chosencmd_requests_msg*)&l->missing_acc_request_buf;
	COUNT_EVENT(PAXOS, l->lec.chosenval_request);
	msg->inst_number[msg->requests_count] = inst_number;
	msg->requests_count += 1;
	
	if(msg->requests_count >= REPEAT_REQUEST_MAX_ENTRIES) {
		net_send_udp(l->mcast_send, msg, FINVAL_REQS_MSG_SIZE(msg), chosenval_request);		
		udp_sender_force_flush(l->mcast_send);
		msg->requests_count = 0;		
	}
}

//Invoked after the missing_map and missing_acceptance may have been
// called to fill gaps, flush send buffer
void post_delivery_check(void * arg) {
	learner_context * l = arg;
	assert(l->initialized);
	
	map_requests_msg * msg1 = (map_requests_msg*)&l->missing_cmd_request_buf;
	chosencmd_requests_msg * msg2 = (chosencmd_requests_msg*)&l->missing_acc_request_buf;

	if(msg1->requests_count > 0) {
		net_send_udp(l->mcast_send, msg1, CMDMAP_REQS_MSG_SIZE(msg1), map_request);		
		msg1->requests_count = 0;
	}
	if(msg2->requests_count > 0) {
		net_send_udp(l->mcast_send, msg2, FINVAL_REQS_MSG_SIZE(msg2), chosenval_request);
		msg2->requests_count = 0;
	}
	
	udp_sender_force_flush(l->mcast_send);
}


void on_mcast_msg(void* data, size_t size, lp_msg_type type, void * arg) {
    
	learner_context * l = arg;
	assert(l->initialized);
	
    switch(type) {
        case command_map: {
            cmdmap_msg* msg = data;
            assert(size == CMDMAP_MSG_SIZE(msg));
            delivery_queue_handle_command_map(l->dq, 
                msg->inst_number, &msg->cmd_key, 
                msg->cmd_size, msg->cmd_value);         
        }
        break;
            
        case acceptance: {
            acceptance_msg* msg = data;
            assert(size == sizeof(acceptance_msg));
            delivery_queue_handle_acceptance(l->dq, msg->inst_number, &msg->cmd_key);
        }
        break;
        
        default: 
        LOG_MSG(DEBUG, ("Dropping message of type %d\n", type));
    }
}

// Invoked after a multicast message has been processed
void post_receive_check(void * arg) {
	learner_context * l = arg;
	assert(l->initialized);
	
	dq_deliver_loop(l->dq);
}

int learner_init(
	const char * config_file_path, 
	custom_init_func cust_init, 
	deliver_callback dcb,
	void * cb_arg,
	learner_context ** learner_ptr)
{
	
	learner_context * l = malloc(sizeof(learner_context));
	assert(l != NULL);
	assert(!l->initialized);
	
	
	l->initialized = false;
	l->mcast_send = NULL;
	l->mcast_recv = NULL;
	l->dq = NULL;
	l->cfg = NULL;
	l->tm = NULL;
	memset(l->missing_cmd_request_buf, '\0', MAX_MESSAGE_SIZE);
	memset(l->missing_acc_request_buf, '\0', MAX_MESSAGE_SIZE);
	
	l->lec.map_request = 0;
	l->lec.chosenval_request = 0;
	
	l->cb_arg = cb_arg;
	
    // Parse configuration file
    int result = config_mngr_init(
        config_file_path, /*Path to config file*/
        0,           /*Numeric ID of this acceptor, -1 since it's a learner*/
        on_configuration_change, /*Callback for config update*/
		l,                       /*cb_arg*/
		&l->cfg                  /*cfg** holder*/
        );
    assert(result == 0);
    LOG_MSG(DEBUG, ("Configuration manager initialized!\n"));

    // Set event for topology change
    result = topology_mngr_init(
        on_topology_change,   /*Callback for ring update*/
		l,
		l->cfg,
		&l->tm
    );
	assert(result == 0);
    LOG_MSG(DEBUG, ("Topology manager initialized!\n"));

    // Create delivery queue
    l->dq = delivery_queue_init( 
		dcb,
		cb_arg,
		on_missing_cmdmap, 
		on_missing_acceptance, 
		post_delivery_check, 
		l,
		l->cfg);
	assert(l->dq != NULL);
	LOG_MSG(DEBUG, ("Delivery queue initialized!\n"));
    
    // Connect to some multicaster to request retransmissions
    //TODO instead of asking everything to the multicaster, should:
    // - ask missed mappings to multicaster
    // - ask final values to some acceptor with TCP
    l->mcast_send = udp_sender_init(
        lptopo_get_leader_addr(l->tm),        /*Remote address string*/
        lptopo_get_leader_ring_port(l->tm),             /*Remote port*/
		l->cfg
        );
	assert(l->mcast_send != NULL);
	//No autoflush, flush triggered in post_delivery_check
	LOG_MSG(DEBUG, ("Socket to Multicaster initialized\n"));
        
    // Open listen port for multicast
    // Set event for multicast messages
    l->mcast_recv = mcast_receiver_init(
        lpconfig_get_mcast_addr(l->cfg),       /*Listen address*/
        lpconfig_get_mcast_port(l->cfg),             /*Listen port*/
        on_mcast_msg, /*Callback for packet received*/
		l, 
		l->cfg
        ); 
    assert(l->mcast_recv != NULL);
	udp_receiver_set_postdeliver_callback(l->mcast_recv, post_receive_check);
	LOG_MSG(DEBUG, ("Multicas receiver initialized\n"));
			
	*learner_ptr = l;
	l->initialized = true;
	
	if(cust_init != NULL) {
		LOG_MSG(DEBUG, ("Invoking custom initialization of base_learner\n"));
		cust_init(cb_arg);
	}

	return 0;
	
}

topolo_mngr * learner_get_topolo_mngr(learner_context * l) {
	assert(l != NULL);
	assert(l->initialized);
	assert(l->tm != NULL);
	return l->tm;
}

config_mngr * learner_get_config_mngr(learner_context * l) {
	assert(l != NULL);
	assert(l->initialized);
	assert(l->cfg != NULL);
	return l->cfg;
}
