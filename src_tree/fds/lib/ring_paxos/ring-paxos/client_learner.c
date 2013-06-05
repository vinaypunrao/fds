#include <stdbool.h>
#include <time.h>
#include <stdio.h>

#include "lp_learner.h"
#include "lp_timers.h"
#include "lp_utils.h"
/*
	This is the simplest application implementing a Paxos learner.
	It is provided as an example.
*/

const bool verbose = false;

typedef struct learner_stats_t {
	iid_t current_inst_number;

	struct timeval print_stats_interval;
	periodic_event * periodic_stats;

	int start_time;
	long unsigned deliver_count;
	long unsigned deliver_bytes;

	int prev_time;
	long unsigned ls_deliver_count;
	long unsigned ls_deliver_bytes;
	
	learner_context * lc;
} learner_stats;

//Triggered when the next Atomic Broadcast value can be delivered
// While executing, the underlying learner is blocked, this function should be quick!
void on_deliver(void* cmd_value, size_t cmd_size, void * arg) {
	
	learner_stats * ls = arg;
	
	ls->ls_deliver_count += 1;
	ls->ls_deliver_bytes += cmd_size;
	
	if(verbose) {
		ls->current_inst_number += 1;
		char * str = cmd_value;
		printf(" >>> DELIVERY %lu (size:%lu)\n", ls->current_inst_number, cmd_size);
		printf("     [%c, %c, %c, ...] <<< \n", str[0], str[1], str[2]);		
	}
}

void print_stats(void * arg) {
	learner_stats * ls = arg;
	if(ls->ls_deliver_count == 0) {
		printf("Nothing delivered\n");
		return;
	}

	int current_time = time(NULL);

	int last_elapsed_secs = current_time - ls->prev_time;
	int last_delivery_rate = ls->ls_deliver_count/last_elapsed_secs;
	int last_delivery_tp = ((ls->ls_deliver_bytes * 8) / (1024*1024)) / last_elapsed_secs;

	printf("\n\nLAST SAMPLE:_________________________\n");
	printf("%lu vals (%lu kbytes) in %d sec\n", ls->ls_deliver_count, (ls->ls_deliver_bytes/1024), last_elapsed_secs);
	printf("%d val/s %d Mbit/s\n", last_delivery_rate, last_delivery_tp);

	ls->prev_time = current_time;	
	ls->deliver_count += ls->ls_deliver_count;
	ls->ls_deliver_count = 0;
	ls->deliver_bytes += ls->ls_deliver_bytes;
	ls->ls_deliver_bytes = 0;
	
	int tot_elapsed_secs = current_time - ls->start_time;
	int tot_delivery_rate = ls->deliver_count/tot_elapsed_secs;
	int tot_delivery_tp = ((ls->deliver_bytes * 8) / (1024*1024)) / tot_elapsed_secs;
	
	printf("TOTAL:_______________________________\n");
	printf("%lu vals (%lu Mbytes) in %d sec\n", ls->deliver_count, (ls->deliver_bytes/(1024*1024)), tot_elapsed_secs);
	printf("%d val/s %d Mbit/s\n", tot_delivery_rate, tot_delivery_tp);
	
	printf("BANDWIDTH:___________________________\n");
	learner_print_eventcounters(ls->lc);
}

void custom_init(void * arg) {
	
	learner_stats * ls = arg;
	
	//Add a periodic event that prints statistics
	ls->periodic_stats =  
	set_periodic_event(
	    &ls->print_stats_interval,  /*Interval for this event*/
	    print_stats, /*Called periodically*/
	    ls /*Argument passed to above function*/
	    );
	
	ls->start_time = time(NULL);
	ls->prev_time = ls->start_time;
	
}

int main (int argc, char const *argv[]) {
    
    print_cmdline_args(argc, argv);
    
    // validate_acceptor_args_or_quit(argc, argv);
    const char * config_file_path = argv[1];
    
    //Second arg is optional, the acceptor to which to connect
    // acceptor_id_t favourite_acc_id = atoi(argv[2]);

	event_init(); //Init libevent 
	
	learner_context * learner;
	learner_stats ls = {0, {5,0}, NULL, 0, 0, 0, 0, 0, 0, NULL};
	
	//Initialize the learner, passing:
	// - configuration file
	// - a custom initialization function (required if you add libevent events)
	// - the function to be triggered on delivery of values
	// - Callback argument
	//Multiple learners sharing the same event loop is ok
	learner_init(config_file_path, custom_init, on_deliver, &ls, &learner);

	ls.lc = learner;
	
	event_dispatch(); //Start libevent loop
	
    return 0;
}
