#include <stdlib.h>
#include <assert.h>

#include "lp_utils.h"
#include "lp_topology.h"

// Description of the current topology
struct topology_info_t {
    void* placeholder;
};

struct topolo_mngr_t {
	config_mngr * cfg;
	
    topology_info * current_topology;
    topo_change_callback cb;
	void * cb_arg;
	
	bool initialized;
};

/*** HELPERS ***/

topology_info * build_topology_info(config_mngr * cfg) {
	UNUSED_ARG(cfg);
	struct topology_info_t * ti = calloc(1, sizeof(struct topology_info_t));
    return ti;
    
}
/*** PUBLIC ***/

int topology_mngr_init(
    topo_change_callback on_topology_change,   /*Callback for ring update*/
	void * callbacks_arg,
	config_mngr * cfg,
    topolo_mngr ** tm_ptr
    )
    {
		*tm_ptr = NULL;

        // topolo_mngr struct handles topology updates
        topolo_mngr * tm = calloc(1, sizeof(topolo_mngr));
        assert(tm != NULL);
        assert(!tm->initialized);
        
        //Save callback for topology update
        tm->cb = on_topology_change;
		tm->cb_arg = callbacks_arg;
		
		tm->cfg = cfg;

        //Build default topology_info from config
        tm->current_topology = build_topology_info(cfg);
        assert(tm->current_topology != NULL);

		*tm_ptr = tm;
		
		tm->initialized = true;
        
        return 0;
    }

acceptor_id_t lptopo_get_successor_id(topolo_mngr * tm) {
    assert(tm->initialized);

    acceptor_id_t successor = lpconfig_get_self_acceptor_id(tm->cfg) + 1;
    if(successor > lpconfig_get_acceptors_count(tm->cfg)) {
        assert (successor == lpconfig_get_acceptors_count(tm->cfg) + 1);
        successor = 1;
    }
    return successor;
}

int lptopo_get_successor_port(topolo_mngr * tm) {
    assert(tm->initialized);

    //TODO low_priority

    acceptor_id_t successor = lptopo_get_successor_id(tm);
    return lpconfig_get_ring_port_of(tm->cfg, successor);
}

char * lptopo_get_successor_addr(topolo_mngr * tm) {
    assert(tm->initialized);

    //TODO low_priority

    acceptor_id_t successor = lptopo_get_successor_id(tm);
    return lpconfig_get_ip_addr_of(tm->cfg, successor);
}

char * lptopo_get_leader_addr(topolo_mngr * tm) {
    assert(tm->initialized);

    //TODO low_priority

    return lpconfig_get_ip_addr_of(tm->cfg, 1);
}

int lptopo_get_leader_ring_port(topolo_mngr * tm) {
    assert(tm->initialized);

    //TODO low_priority

    return lpconfig_get_ring_port_of(tm->cfg, 1);

}

int lptopo_get_leader_clients_port(topolo_mngr * tm) {
    assert(tm->initialized);

    //TODO low_priority

    return lpconfig_get_learners_port_of(tm->cfg, 1);
}
