#include <string.h>
#include <assert.h>

#include "lp_utils.h"
#include "lp_clients_handling.h"
#include "lp_queue.h"
#include "lp_network.h"

struct clival_mngr_t {
    client_submit_cb cb;
	void * cb_arg;
    lpqueue * queue;
	udp_receiver * cli_recv_udp;
	config_mngr * cfg;
	
	bool initialized;
};

static void client_values_mngr_handle_data (void * msg, size_t msg_size, lp_msg_type type, void * arg) {
	if(type != client_submit) {
		LOG_MSG(WARNING, ("Warning, unknow message type %d\n", type));
		return;
	}
	
	assert(arg != NULL);
	clival_mngr * cvm = (clival_mngr*)arg;
	
	assert(cvm->initialized);
	cvm->cb(msg, msg_size, cvm, cvm->cb_arg);
}


bool client_values_mngr_UDP_init(
    client_submit_cb submit_cb, /*Called when a client sends a value*/
	void * cb_arg,
	clival_mngr ** cvm_ptr,
	config_mngr * cfg
    ) {
	
	clival_mngr * cvm;

    // clival_mngr struct handles values sent by clients
    cvm = calloc(1, sizeof(clival_mngr));
    assert(cvm != NULL);
	assert(!cvm->initialized);


    //Save callback invoked on client submission
    cvm->cb = submit_cb;
	cvm->cb_arg = cb_arg;
	
	//Internal queue not initialized by default (TODO why?)
    cvm->queue = NULL;

	cvm->cfg = cfg;

	//Open listen socket for client commands, UDP
	cvm->cli_recv_udp = udp_receiver_init(
		lpconfig_get_ring_inbound_addr(cvm->cfg),
		lpconfig_get_learners_inbound_port(cvm->cfg), 
		client_values_mngr_handle_data, 
		cvm, 
		cvm->cfg);
	assert(cvm->cli_recv_udp != NULL);

	*cvm_ptr = cvm;

	cvm->initialized = true;
	
    return true;
}

bool client_values_mngr_init_queue(clival_mngr * cvm, unsigned queue_length) {
	assert(cvm->initialized);
    if(cvm->queue != NULL) {
        printf("Queue is already initialized in for this values manager!\n");
        return NULL;
    }
    
    cvm->queue = queue_init(queue_length);
    if(cvm->queue == NULL) {
        printf("Failed to create queue!\n");
        return NULL;
    }

    return cvm;
}

bool cvm_get_next_value(clival_mngr * cvm, void ** cmd_value_p, size_t * cmd_size_p) {
	assert(cvm->initialized);
    assert(cvm->queue != NULL);
    return queue_pop(cvm->queue, cmd_value_p, cmd_size_p);
}

bool cvm_save_value(clival_mngr * cvm, void * cmd_value, size_t cmd_size) {
	assert(cvm->initialized);
    assert(cvm->queue != NULL);
    return queue_append(cvm->queue, cmd_value, cmd_size, true);

}
bool cvm_push_back_value(clival_mngr * cvm, void * cmd_value, size_t cmd_size) {
	assert(cvm->initialized);
    assert(cvm->queue != NULL);
    return queue_prepend(cvm->queue, cmd_value, cmd_size, false);
}

unsigned cvm_pending_list_size(clival_mngr * cvm) {
	assert(cvm->initialized);
	return queue_get_current_size(cvm->queue);
}

void cvm_print_bandwidth_stats(clival_mngr * cvm, int time_now) {
	assert(cvm->initialized);
	udp_receiver_print_stats(cvm->cli_recv_udp, time_now);
}
