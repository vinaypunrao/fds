#include "lp_delivery_queue.h"
#include "lp_topology.h"

//Learner handle seen by client
typedef struct learner_t learner_context;

//A custom initialization for the learner, useful for adding
// events to the libevent loop
typedef void(*custom_init_func)(void*);

// Initializes a learner in the current process
//Warning! this must be called 
// AFTER event_init and BEFORE event_dispatch
int learner_init(const char * config_file_path, custom_init_func cust_init, deliver_callback dcb, void * cb_arg, learner_context ** learner_ptr);

//Prints statistic of learner events
//i.e. message loss, retransmission requests, etc
void learner_print_eventcounters(learner_context * l);

void learner_delayed_start(learner_context * l);

topolo_mngr * learner_get_topolo_mngr(learner_context * l);
config_mngr * learner_get_config_mngr(learner_context * l);