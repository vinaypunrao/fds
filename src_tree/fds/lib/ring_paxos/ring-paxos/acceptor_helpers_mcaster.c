
void mcaster_clear_counters(acceptor * acc) {
	memset(&acc->mec, '\0', sizeof(struct mcaster_event_counters));
	acc->mec.last_print_time = time(NULL);
}

void mcaster_print_counters(void * arg) {
	acceptor * acc = arg;
	
	printf("*** Multicaster counters *** \n");
	PRINT_COUNT(acc->mec.p1_timeout);
	PRINT_COUNT(acc->mec.p2_timeout);
	PRINT_COUNT(acc->mec.p2_waits_p1);
	PRINT_COUNT(acc->mec.out_of_values);
	PRINT_COUNT(acc->mec.dropped_client_values);
	PRINT_COUNT(acc->mec.p2_window_full);
	PRINT_COUNT(acc->mec.p1_range_try);
	PRINT_COUNT(acc->mec.p1_range_success);
	PRINT_COUNT(acc->mec.chosenval_request);
	PRINT_COUNT(acc->mec.map_request);
	PRINT_COUNT(acc->mec.map_request_ignored);
	
	PRINT_COUNT(acc->highest_closed_iid);
	PRINT_COUNT(acc->highest_open_iid);
	printf("Pending client values:%u\n", cvm_pending_list_size(acc->cvm));
	PRINT_COUNT_DISTRIBUTION(acc->mec.concurrent_p2_open, (unsigned)lpconfig_get_max_p2_open_per_iteration(acc->cfg));
	
	int time_now = time(NULL);
	printf("Multicast BW:\n");
	udp_sender_print_stats(acc->mcast_send, time_now);
	printf("Ring BW:\n");
	udp_sender_print_stats(acc->succ_send, time_now);
	udp_receiver_print_stats(acc->pred_recv, time_now);
	printf("Clients BW:\n");
	cvm_print_bandwidth_stats(acc->cvm, time_now);	
	printf("----------------------------- \n\n");
}

void mcaster_update_wallclock(void * arg) {
	acceptor * acc = arg;
    //Update the current clock
    // This function is triggered
    // - before any network receive (not between callbacks for individual messages)
    // - before the periodic check that calls all periodic checks below
    gettimeofday(&acc->mcaster_clock, NULL);
}

void mcaster_do_phase1(acceptor * acc, mcaster_instance_record * mir) {
    assert(mir->ballot != 0);
    
    // Executes phase 1 using the stored ballot
    LOG_MSG(PAXOS, ("Executing phase 1 for inst:%lu, bal:%u\n", 
        mir->inst_number, mir->ballot));
    
    //Create and send P1 message
    phase1_msg m;
    m.inst_number = mir->inst_number;
    m.ballot = mir->ballot;
    m.promises_count = 0;
    m.highest_accepted_ballot = 0;
	m.highest_promised_ballot = 0;
    m.cmd_size = 0;
    net_send_udp(acc->succ_send, &m, PH1_MSG_SIZE_S(m), phase1);
    
    //Update structure
    timer_set_timeout(&acc->mcaster_clock, &mir->timeout, lpconfig_get_p1_interval(acc->cfg));
    mir->status = p1_pending;
}

void mcaster_do_phase1_range(acceptor * acc, iid_t from, iid_t to, ballot_t b) {
	phase1_range_msg m;

	m.from = from;
	m.to = to;
	m.ballot = b;
	m.promises_count = 0;
    net_send_udp(acc->succ_send, &m, sizeof(phase1_range_msg), phase1_range);
	
	COUNT_EVENT(PAXOS, acc->mec.p1_range_try);

}

void mcaster_do_phase2(acceptor * acc, mcaster_instance_record * mir) {
    phase2_msg msg; //TSAFE Remove
    command_id * dest = &msg.cmd_key;
    command_id * src = &mir->assigned_cmd_key;

    //Create and send p2_message
    msg.inst_number = mir->inst_number;
    msg.ballot = mir->ballot;
    msg.accepts_count = 0;
    CMD_KEY_COPY(dest, src);
    net_send_udp(acc->mcast_send, &msg, sizeof(phase2_msg), phase2);
    
    //Save state into instance record
    mir->status = p2_pending;
    timer_set_timeout(&acc->mcaster_clock, &mir->timeout, lpconfig_get_p2_interval(acc->cfg));
}

void mcaster_broadcast_mapping(acceptor * acc, mcaster_instance_record * mir) {
    char map_msg_buf[MAX_MESSAGE_SIZE]; //TSAFE Remove

	char str[32];
	LOG_MSG(PAXOS, ("Broadcasting mapping of instance:%lu, key:%s\n",
		mir->inst_number, print_cmd_key(&mir->assigned_cmd_key, str)));

    // Create a cmd_map message consisting of
    // command identifier, command value, instance number (in which it will be proposed)
    cmdmap_msg * msg = (cmdmap_msg*)&map_msg_buf;
    command_id * src = &mir->assigned_cmd_key;
    command_id * dest = &msg->cmd_key;

    msg->inst_number = mir->inst_number;
    msg->cmd_size = mir->assigned_cmd_size;
    CMD_KEY_COPY(dest, src);
    memcpy(msg->cmd_value, mir->assigned_cmd_value, mir->assigned_cmd_size);
    
    //And broadcast it to all acceptors
    net_send_udp(acc->mcast_send, msg, CMDMAP_MSG_SIZE(msg), command_map);
	timer_set_timeout(&acc->mcaster_clock, &mir->repeat_cmdmap_timeout, lpconfig_get_retransmit_request_interval(acc->cfg));
}

void mcaster_broadcast_acceptance(acceptor * acc, mcaster_instance_record * mir) {
	assert(mir->status == done);
    //Broadcast the fact that some value (identifier) was chosen (consensus)
    //for some instance
    acceptance_msg msg; //TSAFE Remove
    LOG_MSG(PAXOS, ("Broadcasting acceptance of inst:%lu\n", 
        mir->inst_number));
    msg.inst_number = mir->inst_number;
    
    command_id * src = &mir->assigned_cmd_key;
    command_id * dst = &msg.cmd_key;
    CMD_KEY_COPY(dst, src);
    net_send_udp(acc->mcast_send, &msg, sizeof(acceptance_msg), acceptance);
}

void mcaster_open_new_instances_P1(acceptor * acc) {
	unsigned active_count = acc->p1_ready_count + acc->p1_ready_count;
	
	//More than half of the window size are active,
	// wait before opening more
	if(active_count >= (unsigned)lpconfig_get_preexecution_window_size(acc->cfg)/2) {
		LOG_MSG(PAXOS_DBG, ("Too many are still active (%u + %u)\n", 
			acc->p1_ready_count, acc->p1_ready_count))
		return;
	}
	
	mcaster_instance_record * mir;
	iid_t from, to;
	unsigned to_open = (lpconfig_get_preexecution_window_size(acc->cfg) - active_count);	
	//Ballot used for all new instances
	ballot_t b = make_first_ballot(lpconfig_get_self_acceptor_id(acc->cfg), lpconfig_get_incarnation_number(acc->cfg));

	//Bounds for instances to open now
	from = acc->p1_highest_open + 1;
	to = acc->p1_highest_open + to_open;
	LOG_MSG(PAXOS_DBG, ("Opening phase 1 for instances range [%lu...%lu] (%u)\n", 
		from, to, to_open));
	
	unsigned i;
	for(i = from; i <= to; i++) {
		mir = mcaster_storage_get(acc->msm, i);
		assert(mir->inst_number == i && mir->status == ready);
		
		//Update structure 
		mir->status = p1_pending;
		mir->ballot = b;
	    timer_set_timeout(&acc->mcaster_clock, &mir->timeout, lpconfig_get_p1_interval(acc->cfg));
	}
	
	mcaster_do_phase1_range(acc, from, to, b);
	
	//Update highest open
	acc->p1_highest_open += to_open;
	//Update pending counter
	acc->p1_ready_count += to_open;
	
	LOG_MSG(PAXOS_DBG, ("P1: p1_highest_open:%lu, p1_pending_count:%u\n", acc->p1_highest_open, acc->p1_ready_count))
}

// Iterate over instances in phase 1 and 2, retry the timed-out ones
// (in this case it retries with the same ballot. Ballot is changed
// only if an acceptor tells us that a greater one was already accepted/promised to)
void mcaster_check_expired_instances(acceptor * acc) {

    mcaster_instance_record * mir;

	iid_t upper_bound = IID_MAX(acc->highest_open_iid, acc->p1_highest_open);

    iid_t i;
    for (i = (acc->highest_closed_iid+1); i <= upper_bound; i++) {
        mir = mcaster_storage_get(acc->msm, i);
	    assert(mir != NULL);
	    assert(mir->inst_number == i);
        
        switch(mir->status) {
            case p1_pending: 
                //This instance is expired, execute phase 1 again (same ballot)
                if(timer_is_expired(&mir->timeout, &acc->mcaster_clock)) {
                    LOG_MSG(TIMERS, ("Inst:%lu P1 timed-out\n", mir->inst_number));
					COUNT_EVENT(PAXOS, acc->mec.p1_timeout);
                    mcaster_do_phase1(acc, mir);
                } else {
                    LOG_MSG(TIMERS, ("Inst:%lu P1 not expired yet\n", mir->inst_number));
				}
                break;
            
            case p2_pending: 
                //This instance is expired, execute phase 2 again (same ballot)
                //after repeating the key-value-mapping
                if(timer_is_expired(&mir->timeout, &acc->mcaster_clock)) {
                    LOG_MSG(TIMERS, ("Inst:%lu P2 timed-out\n", mir->inst_number));
					COUNT_EVENT(PAXOS, acc->mec.p2_timeout);
                    mcaster_broadcast_mapping(acc, mir);
                    mcaster_do_phase2(acc, mir);
                } else {
					LOG_MSG(TIMERS, ("Inst:%lu P2 not expired yet\n", mir->inst_number));
				}
                break;
            
            default:
                LOG_MSG(PAXOS_DBG, ("Inst:%lu not pending\n", mir->inst_number));
        }
    }
};
    
// Start phase 2 to deliver values/commands submitted by clients
void mcaster_open_new_instances_P2(acceptor * acc) {
        
    unsigned active_instances_range_size = lpconfig_get_max_active_instances(acc->cfg);
    
    void * client_cmd_value;
    size_t client_cmd_size;
    mcaster_instance_record * mir;
    
	int open_count = 0;
	bool is_window_full = ((acc->highest_open_iid - acc->highest_closed_iid) >= active_instances_range_size);
	if(is_window_full) {
		COUNT_EVENT(PAXOS, acc->mec.p2_window_full);
	}
		
    // Keep opening new instances until we hit range limit or we run out of client values
    while((active_instances_range_size - (acc->highest_open_iid - acc->highest_closed_iid)) > 0) {
		iid_t current_iid = acc->highest_open_iid+1;
		
		//Instances beyond p1_highest_open are for sure not ready yet...
		if(current_iid > acc->p1_highest_open) {
			LOG_MSG(PAXOS, ("Cannot start phase2 for inst:%lu, p1 not run yet\n", current_iid));
			COUNT_EVENT(PAXOS, acc->mec.p2_waits_p1);
			break;
		}
		
		//No instance is ready to start phase 2
		if(acc->p1_ready_count == 0) {
			LOG_MSG(PAXOS, ("Cannot start phase2 for inst:%lu, out of p1_ready instances\n", current_iid));
			COUNT_EVENT(PAXOS, acc->mec.p2_waits_p1);
			break;			
		}
		
		//Retrieve instance info
	    mir = mcaster_storage_get(acc->msm, current_iid);
	    assert(mir != NULL);
	    assert(mir->inst_number == current_iid);
		
		//Can't begin phase 2 while phase 1 is still pending
		if(mir->status != p1_done) {
			assert(mir->status == p1_pending);
			COUNT_EVENT(PAXOS, acc->mec.p2_waits_p1);
			LOG_MSG(PAXOS, ("Cannot start phase2 for inst:%lu, still p1_pending\n", mir->inst_number));
			break;
		}
		
		//Check that there are client values to send
		assert(mir->ballot != 0);
        if(cvm_get_next_value(acc->cvm, &client_cmd_value, &client_cmd_size) == false) {
			COUNT_EVENT(PAXOS, acc->mec.out_of_values);
            LOG_MSG(DEBUG, ("Out of client values to submit!\n"));
            break;
        }
        //Assign the value to the next unused instance
        mcaster_storage_assign_value(acc->msm, mir, client_cmd_value, client_cmd_size);
        
		LOG_MSG(PAXOS, ("Opening new instance:%ld\n", current_iid));
		acc->p1_ready_count -= 1;
        
        //Broadcast value and execute phase 2 for it
        mcaster_broadcast_mapping(acc, mir);
        mcaster_do_phase2(acc, mir);

        acc->highest_open_iid += 1;
		open_count += 1;
		if(open_count >= lpconfig_get_max_p2_open_per_iteration(acc->cfg)) {
			break;
		}
		
    }
	COUNT_EVENT_DISTRIBUTION(acc->mec.concurrent_p2_open, open_count);
};

void mcaster_handle_phase1_range_msg(acceptor * acc, phase1_range_msg* msg, size_t size) {
	assert(size == sizeof(phase1_range_msg));
	
	//Someone else's ballot, do nothing
    if(!is_my_ballot(msg->ballot, 
        lpconfig_get_self_acceptor_id(acc->cfg), 
        lpconfig_get_incarnation_number(acc->cfg))) {
        LOG_MSG(PAXOS_DBG, ("Discarding p1 message, not my ballot\n"));
        return;        
    }
    
    //Message is relative to old instance already closed
	assert(msg->from > acc->highest_closed_iid);
	
	//Not enough promises, retry
    if(msg->promises_count < lpconfig_get_quorum_size(acc->cfg)) {
        LOG_MSG(PAXOS, ("Not enough promises, retrying phase 1 range [%lu...%lu]\n", 
            msg->from, msg->to));
		mcaster_do_phase1_range(acc, msg->from, msg->to, msg->ballot);
        return;
    }

	//Phase 1 successful for all instances in range
	unsigned count = (msg->to - msg->from)+1;
	acc->p1_ready_count -= count;
	acc->p1_ready_count += count;

	LOG_MSG(PAXOS_DBG, ("Completed phase 1 for instances range [%lu...%lu] (%u)\n", 
		msg->from, msg->to, count));
	COUNT_EVENT(PAXOS, acc->mec.p1_range_success);
		
	mcaster_instance_record * mir;	
	unsigned i;
	for(i = msg->from; i <= msg->to; i++) {
		mir = mcaster_storage_get(acc->msm, i);
		assert(mir->inst_number == i && mir->status == p1_pending);
		
		//Update structure 
		mir->status = p1_done;
	}	
}

void mcaster_handle_phase1_msg(acceptor * acc, phase1_msg* msg, size_t size) {
    assert(size == PH1_MSG_SIZE(msg));

    //Message is relative to old instance already closed
    if(msg->inst_number <= acc->highest_closed_iid) {
        LOG_MSG(PAXOS_DBG, ("Discarding message for closed instance %lu\n", 
            msg->inst_number));
        return;
    }
    
    //Someone else's ballot, do nothing
    if(!is_my_ballot(msg->ballot, 
        lpconfig_get_self_acceptor_id(acc->cfg), 
        lpconfig_get_incarnation_number(acc->cfg))) {
        LOG_MSG(PAXOS_DBG, ("Discarding p1 message, not my ballot\n"));
        return;        
    }

    //Retrieve instance information
    mcaster_instance_record * mir = mcaster_storage_get(acc->msm, msg->inst_number);
    assert(mir != NULL);
    assert(mir->inst_number == msg->inst_number);
    
    //Instance is not phase 1 pending, do nothing
    if(mir->status != p1_pending) {
        LOG_MSG(PAXOS_DBG, ("Discarding p1 message for non-pending inst:%lu\n", 
            mir->inst_number));
        return;
    }
    
    //Ballot is not the latest sent, do nothing
    if(mir->ballot != msg->ballot) {
        LOG_MSG(PAXOS_DBG, ("Discarding p1 message, ballot does not match\n"));
        return;
    }

    //Not enough promises, retry
    if(msg->promises_count < lpconfig_get_quorum_size(acc->cfg)) {
        LOG_MSG(PAXOS, ("Not enough promises, retrying phase 1 inst:%lu\n", 
            msg->inst_number));

		//If some acceptor already promised to some higher ballot than the one we're using, 
		// it's pointless to retry with the same, we must increase the ballot to use
		if(msg->highest_promised_ballot != 0) {
			mir->ballot = raise_ballot(mir->ballot, msg->highest_promised_ballot);
		}
		
        mcaster_do_phase1(acc, mir);
        return;
    }
    
    //Phase 1 completed successfully!
	acc->p1_ready_count -= 1;
	acc->p1_ready_count += 1;
	
	mir->status = p1_done;

	if(mir->assigned_cmd_value == NULL) {
	//No assigned value means that we are just pre-executing, leave the instance there
	// it will be used later	
		LOG_MSG(PAXOS, ("Phase 1 completed for inst %lu\n", mir->inst_number));
    } else  {
	//If this instance has an assigned value, it means we already executed phase 2, 
	// which failed and we were forced to go again through phase1. 
	// Since future instances may be waiting on this one, do phase2 right away
	// (after figuring out which command value we should use)
	
		//Acceptors did not accept any value yet, we can send any command
		if(msg->highest_accepted_ballot == 0) {
			LOG_MSG(PAXOS, ("Phase 1 completed without any value, inst:%ld\n",
				mir->inst_number));
			mcaster_do_phase2(acc, mir);
			return;
		}

		command_id * proposed_key = &mir->assigned_cmd_key;
		command_id * returned_key = &msg->cmd_key;

		//Some acceptors accepted, but it's the command that this multicaster proposed
		if(CMD_KEY_EQUALS(proposed_key, returned_key)) {
			LOG_MSG(PAXOS, ("Phase 1 completed with our value, inst:%ld",
				mir->inst_number));
			mcaster_do_phase2(acc, mir);
			return;
		}

		//Acceptors accepted some command (not from this multicaster)
		// We have to drop our value and continue to phase 2 with the value
		// contained in the message

		LOG_MSG(PAXOS, ("Phase 1 completed and returned value not proposed by us, inst:%lu\n", 
			msg->inst_number));

		mcaster_storage_replace_assigned_value(acc->msm, mir, &msg->cmd_key, &msg->cmd_value, msg->cmd_size);
		mcaster_do_phase2(acc, mir);
	}   
};

//A phase 2 message is received that went around the ring.
void mcaster_handle_phase2_msg(acceptor * acc, phase2_msg* msg, size_t size) {
    assert(size == sizeof(phase2_msg));
    
    //Message is relative to old instance already closed
    if(msg->inst_number <= acc->highest_closed_iid) {
        LOG_MSG(PAXOS_DBG, ("Discarding message for closed instance %lu\n", 
            msg->inst_number));
        return;
    }

    mcaster_instance_record * mir = mcaster_storage_get(acc->msm, msg->inst_number);
    assert(mir != NULL);
    assert(mir->inst_number == msg->inst_number);
    
    //Instance is not in status p2_pending, discard the message
    if(mir->status != p2_pending) {
        LOG_MSG(PAXOS_DBG, ("Got P2 msg, instance is not p2_pending, inst:%lu\n",
            msg->inst_number));
        return;
    }
    
    //Ballot is not ours or not the last one, discard the message
    if(mir->ballot != msg->ballot) {
        LOG_MSG(PAXOS_DBG, ("Got P2 message whose ballot does not match, inst:%lu\n",
            msg->inst_number));
        return;
    }
    
    //Ballot is ours, so should be command identifier!
    command_id * assigned_key = &mir->assigned_cmd_key;
    command_id * returned_key = &msg->cmd_key;
    assert(CMD_KEY_EQUALS(assigned_key, returned_key));

    //Not all the acceptors may have accepted
    if(msg->accepts_count < lpconfig_get_quorum_size(acc->cfg)) {
        // Broadcast again the mapping and retry phase 2
        mcaster_broadcast_mapping(acc, mir);
        mcaster_do_phase2(acc, mir);
        return;
    }
    
    //We reached the necessary number of accepts!
    LOG_MSG(PAXOS, ("Inst:%lu closed successfully!\n", msg->inst_number));

	mir->status = done;
    mcaster_broadcast_acceptance(acc, mir);
	
	while(mir->status == done && mir->inst_number == (acc->highest_closed_iid+1)) {
    	//Update range of currently open instances
        acc->highest_closed_iid += 1;
		LOG_MSG(PAXOS_DBG, ("Pending instances lower bound is now %lu\n",
			acc->highest_closed_iid));
		mir = mcaster_storage_get(acc->msm, acc->highest_closed_iid+1);
		assert(mir != NULL);
	    assert(mir->inst_number == acc->highest_closed_iid+1);
	}
};

void mcaster_handle_map_request(acceptor * acc, map_requests_msg* msg, size_t size) {
	assert(size ==CMDMAP_REQS_MSG_SIZE(msg));
	LOG_MSG(PAXOS, ("Learner requested mapping of %u instances\n", 
		msg->requests_count));
	
	unsigned i;
	for(i = 0; i < msg->requests_count; i++) {
		COUNT_EVENT(PAXOS, acc->mec.map_request);
		mcaster_instance_record * mir = mcaster_storage_get(acc->msm, msg->inst_number[i]);
	    assert(mir != NULL);
	    assert(mir->inst_number == msg->inst_number[i]);
	
		//To avoid re-broadcasting multiple times the same request
		// (if multiple processes lost it)
		//A minimum time has to pass between retransmissions
		if(timer_is_expired(&mir->repeat_cmdmap_timeout, &acc->mcaster_clock)) {
			//This function also sets the timeout for the next allowed retransmission
			mcaster_broadcast_mapping(acc, mir);
		} else {	
			COUNT_EVENT(PAXOS, acc->mec.map_request_ignored);
			LOG_MSG(PAXOS_DBG, ("Request for mapping ignored, already sent recently (Inst:%lu)\n",
				mir->inst_number));
		}
	
	}
}

void mcaster_handle_chosenval_request(acceptor * acc, chosencmd_requests_msg* msg, size_t size) {
	assert(size == FINVAL_REQS_MSG_SIZE(msg));
	LOG_MSG(PAXOS, ("Learner requested chosen value of %u instances\n",
		msg->requests_count));
		
	unsigned i;
	for(i = 0; i < msg->requests_count; i++) {
		COUNT_EVENT(PAXOS, acc->mec.chosenval_request);
		mcaster_instance_record * mir = mcaster_storage_get(acc->msm, msg->inst_number[i]);
	    assert(mir != NULL);
	    assert(mir->inst_number == msg->inst_number[i]);

		if(mir->status == done) {
			mcaster_broadcast_acceptance(acc, mir);
		} else {
			LOG_MSG(PAXOS_DBG, ("Final value is not chosen yet for inst:%lu\n", msg->inst_number[i]));
		}
	}	
}

// Called when a client submits a value, may return false if the queue
// is already too long and the value is discardeds
bool mcaster_handle_value_submit(void* msg, size_t size, clival_mngr * cvm, void * arg) {
	acceptor * acc = arg;
	assert(cvm == acc->cvm);

	submit_cmd_msg * smsg = msg;
	assert(SUBMIT_CMD_MSG_SIZE(smsg) == size);
    LOG_MSG(PAXOS, ("Received new client command of size %lu\n", smsg->cmd_size));
	bool enqueued = cvm_save_value(cvm, smsg->cmd_value, smsg->cmd_size);
	if (!enqueued) {
		COUNT_EVENT(PAXOS, acc->mec.dropped_client_values);
	}
	return enqueued;
}

void on_mcaster_periodic_check(void * arg) {
	acceptor * acc = arg;

    LOG_MSG(DEBUG, ("Executing multicaster periodic check\n"));

	//Make sure some number of future instances already completed phase1
    mcaster_update_wallclock(acc);
	mcaster_open_new_instances_P1(acc);
    
    // Iterate over instances in phase 1, retry the timed-out ones
    // Iterate over instances in phase 2, retry the timed-out ones
    mcaster_update_wallclock(acc);
    mcaster_check_expired_instances(acc);
    
    // Start phase 2 to deliver values/commands submitted by clients
    mcaster_update_wallclock(acc);
    mcaster_open_new_instances_P2(acc);
    
    // If any send buffer has data in it, flush it now
    udp_sender_force_flush(acc->mcast_send);
    udp_sender_force_flush(acc->succ_send);
}
