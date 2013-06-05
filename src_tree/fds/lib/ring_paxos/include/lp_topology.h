#ifndef LP_TOPOLOGY_H_9RRA6C3S
#define LP_TOPOLOGY_H_9RRA6C3S

#include <stdbool.h>

#include "paxos_config.h"
#include "lp_config_parser.h"

//This part is stubbed and not completely implemented yet
// This object is responsible of maintaining the current topology for the ring

// Description of the current topology
struct topology_info_t;
typedef struct topology_info_t topology_info;

struct topolo_mngr_t;
typedef struct topolo_mngr_t topolo_mngr;

typedef void(*topo_change_callback)(topology_info*, void*);

int topology_mngr_init(
        topo_change_callback on_topology_change,   /*Callback for ring update*/
		void * callbacks_arg,
		config_mngr * cfg,
        topolo_mngr ** tm_ptr
    );

// Get the name of successor in the ring
acceptor_id_t lptopo_get_successor_id(topolo_mngr * tm);

// Get address and port number of successor in the ring
char * lptopo_get_successor_addr(topolo_mngr * tm);
int lptopo_get_successor_port(topolo_mngr * tm);

// Get the address and port of Paxos leader
char * lptopo_get_leader_addr(topolo_mngr * tm);
int lptopo_get_leader_ring_port(topolo_mngr * tm);

//Get port number where to submit client values
// and retransmission requests
int lptopo_get_leader_clients_port(topolo_mngr * tm);


#endif /* end of include guard: LP_TOPOLOGY_H_9RRA6C3S */
