#ifndef LP_CLIENTS_HANDLING_H_TCEXS18
#define LP_CLIENTS_HANDLING_H_TCEXS18

#include <stdbool.h>
#include <stdlib.h>

#include "lp_config_parser.h"

//Used by Paxos leader to manage incoming client values
typedef struct clival_mngr_t clival_mngr;

//Callback triggered when a value is submitted
typedef bool(*client_submit_cb)(void*,size_t,clival_mngr*, void*);

//Initialize a client values manager
//receiving submissions through UDP
bool client_values_mngr_UDP_init(
    client_submit_cb submit_cb, /*Called when a client sends a value*/
	void * cb_arg,
    clival_mngr ** cvm_ptr,
    config_mngr * cfg
    );

//Initialize automatic queue management of
// client values manager
bool client_values_mngr_init_queue(
    clival_mngr * cvm, 
    unsigned queue_length
    );

//Pop next value in client values queue
bool cvm_get_next_value(clival_mngr * cvm, void ** cmd_value_p, size_t * cmd_size_p);

//Stores the received values in the queue, if previously initialized
bool cvm_save_value(clival_mngr * cvm, void * cmd_value, size_t cmd_size);

//Re-enqueue a value that was previously popped 
bool cvm_push_back_value(clival_mngr * cvm, void * cmd_value, size_t cmd_size);

//Get the current size of pending values queue
unsigned cvm_pending_list_size(clival_mngr * cvm);

//Prints statistics about the quantity of values received from clients
void cvm_print_bandwidth_stats(clival_mngr * cvm, int time_now);


#endif /* end of include guard: LP_CLIENTS_HANDLING_H_TCEXS18 */
