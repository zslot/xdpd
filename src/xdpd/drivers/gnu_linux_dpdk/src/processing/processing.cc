#include "processing.h"
#include <utils/c_logger.h>
#include <rte_cycles.h>
#include <rte_spinlock.h>
#include <sstream>
#include <iomanip>

#include "assert.h"
#include "../util/compiler_assert.h"
#include "../io/rx.h"
#include "../io/tx.h"

#include "../io/port_state.h"
#include "../io/iface_manager.h"
#include <rofl/datapath/pipeline/openflow/of_switch.h>

//Pool sizes
extern unsigned int mbuf_pool_size;

//Wrong CPU socket overhead weight
#define WRONG_CPU_SOCK_OH 0x80000000;
#define POOL_MAX_LEN_NAME 32

using namespace xdpd::gnu_linux_dpdk;

//
// Processing state
//
static unsigned int max_cores;
static rte_spinlock_t mutex;
core_tasks_t processing_core_tasks[RTE_MAX_LCORE];
unsigned int total_num_of_phy_ports = 0;
unsigned int total_num_of_nf_ports = 0;
unsigned int running_hash = 0;


struct rte_mempool* direct_pools[MAX_CPU_SOCKETS];
struct rte_mempool* indirect_pools[MAX_CPU_SOCKETS];

/*
* Initialize data structures for processing to work
*/
rofl_result_t processing_init(void){

	unsigned int i;
	struct rte_config* config;
	enum rte_lcore_role_t role;
	unsigned int sock_id;
	char pool_name[POOL_MAX_LEN_NAME];

	//Cleanup
	memset(direct_pools, 0, sizeof(direct_pools));
	memset(indirect_pools, 0, sizeof(indirect_pools));
	memset(processing_core_tasks,0,sizeof(core_tasks_t)*RTE_MAX_LCORE);

	//Initialize basics
	config = rte_eal_get_configuration();
	max_cores = config->lcore_count;
	rte_spinlock_init(&mutex);

	XDPD_DEBUG(DRIVER_NAME"[processing] Processing init: %u logical cores guessed from rte_eal_get_configuration(). Master is: %u\n", config->lcore_count, config->master_lcore);

	//Define available cores
	for(i=0; i < RTE_MAX_LCORE; ++i){
		role = rte_eal_lcore_role(i);
		if(role == ROLE_RTE){

			if(i != config->master_lcore){
				processing_core_tasks[i].available = true;
				XDPD_DEBUG(DRIVER_NAME"[processing] Marking core %u as available\n",i);
			}

			//Recover CPU socket for the lcore
			sock_id = rte_lcore_to_socket_id(i);

			if(direct_pools[sock_id] == NULL){

				/**
				*  create the mbuf pool for that socket id
				*/
				snprintf (pool_name, POOL_MAX_LEN_NAME, "pool_direct_%u", sock_id);
				XDPD_INFO(DRIVER_NAME"[processing] Creating %s with #mbufs %u for CPU socket %u\n", pool_name, mbuf_pool_size, sock_id);

				direct_pools[sock_id] = rte_mempool_create(
					pool_name,
					mbuf_pool_size,
					MBUF_SIZE, 32,
					sizeof(struct rte_pktmbuf_pool_private),
					rte_pktmbuf_pool_init, NULL,
					rte_pktmbuf_init, NULL,
					sock_id, 0);

				if (direct_pools[sock_id] == NULL)
					rte_panic("Cannot init direct mbuf pool for CPU socket: %u\n", sock_id);

//Softclonning is disabled
#if 0
				snprintf (pool_name, POOL_MAX_LEN_NAME, "pool_indirect_%u", sock_id);
				XDPD_INFO(DRIVER_NAME"[processing] Creating %s with #mbufs %u for CPU socket %u\n", pool_name, mbuf_pool_size, sock_id);
				indirect_pools[sock_id] = rte_mempool_create(
						pool_name,
						mbuf_pool_size,
						sizeof(struct rte_mbuf), 32,
						0,
						NULL, NULL,
						rte_pktmbuf_init, NULL,
						sock_id, 0);

				if(indirect_pools[sock_id] == NULL)
					rte_panic("Cannot init indirect mbuf pool for CPU socket: %u\n", sock_id);
#else
				//Avoid compiler to complain
				(void)indirect_pools;
#endif
			}

		}
	}

	//Print the status of the cores
	processing_dump_core_states();

	return ROFL_SUCCESS;
}


/*
* Destroy data structures for processing to work
*/
rofl_result_t processing_destroy(void){

	unsigned int i;

	XDPD_DEBUG(DRIVER_NAME"[processing] Shutting down all active cores\n");

	//Stop all cores and wait for them to complete execution tasks
	for(i=0;i<RTE_MAX_LCORE;++i){
		if(processing_core_tasks[i].available && processing_core_tasks[i].active){
			XDPD_DEBUG(DRIVER_NAME"[processing] Shutting down active core %u\n",i);
			processing_core_tasks[i].active = false;
			//Join core
			rte_eal_wait_lcore(i);
		}
	}
	return ROFL_SUCCESS;
}

//Synchronization code
static void processing_wait_for_cores_to_sync(){

	unsigned int i;

	for(i=0;i<RTE_MAX_LCORE;++i){
		if(processing_core_tasks[i].active){
			while(processing_core_tasks[i].running_hash != running_hash);
		}
	}
}

int processing_core_process_packets(void* not_used){

	unsigned int i, l, core_id;
	int j;
	bool own_port;
	switch_port_t* port;
	port_bursts_t* port_bursts;
        uint64_t diff_tsc, prev_tsc;
	struct rte_mbuf* pkt_burst[IO_IFACE_MAX_PKT_BURST]={0};
	core_tasks_t* tasks = &processing_core_tasks[rte_lcore_id()];

	//Time to drain in tics
	const uint64_t drain_tsc = (rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S * IO_BURST_TX_DRAIN_US;

	//Own core
	core_id = rte_lcore_id();

	//Parsing and pipeline extra state
	datapacket_t pkt;
	datapacket_dpdk_t* pkt_state = create_datapacket_dpdk(&pkt);

	//Init values and assign
	pkt.platform_state = (platform_datapacket_state_t*)pkt_state;
	pkt_state->mbuf = NULL;

	//Set flag to active
	tasks->active = true;

	//Last drain tsc
	prev_tsc = 0;

	while(likely(tasks->active)){

		//Update running_hash
		tasks->running_hash = running_hash;

		//Calc diff
		diff_tsc = prev_tsc - rte_rdtsc();

		//Drain TX if necessary
		if(unlikely(diff_tsc > drain_tsc)){

			//Handle physical ports
			for(i=0, l=0; l<total_num_of_phy_ports && likely(i<PROCESSING_MAX_PORTS) ; ++i){

				if(!tasks->phy_ports[i].present)
					continue;

				l++;

				//make code readable
				port_bursts = &tasks->phy_ports[i];

				//Check whether is our port (we have to also transmit TX queues)
				own_port = (port_bursts->core_id == core_id);

				//Flush (enqueue them in the RX/TX port lcore)
				for( j=(IO_IFACE_NUM_QUEUES-1); j >=0 ; j-- ){
					flush_port_queue_tx_burst(phy_port_mapping[i], i, &port_bursts->tx_queues_burst[j], j);

					if(own_port)
						transmit_port_queue_tx_burst(i, j, pkt_burst);
				}
			}

#ifdef GNU_LINUX_DPDK_ENABLE_NF
			//handle NF ports
			for(i=0, l=0; l<total_num_of_nf_ports && likely(i<PROCESSING_MAX_PORTS) ; ++i){

				if(!tasks->nf_ports[i].present)
					continue;

				l++;

				//make code readable
				port_bursts = &tasks->nf_ports[i];

				if(nf_port_mapping[i]->type == PORT_TYPE_NF_EXTERNAL){
					//Check whether is our port (we have to also transmit TX queues)
					own_port = (port_bursts->core_id == core_id);

					flush_kni_nf_port_burst(nf_port_mapping[i], i, &port_bursts->tx_queues_burst[0]);

					if(own_port)
						transmit_kni_nf_port_burst(nf_port_mapping[i], i, pkt_burst);
				}else{
					assert(nf_port_mapping[i]->type == PORT_TYPE_NF_SHMEM);
					port = nf_port_mapping[i];
					flush_shmem_nf_port(port, ((dpdk_shmem_port_state_t*)port->platform_port_state)->to_nf_queue, &port_bursts->tx_queues_burst[0]);
				}
			}
#endif
		}

		//Process RX
		for(i=0;i<tasks->num_of_rx_ports;++i)
		{
			port = tasks->port_list[i];
			if(likely(port != NULL) && likely(port->up)){ //This CAN happen while deschedulings
				//Process RX&pipeline
				process_port_rx(core_id, port, pkt_burst, &pkt, pkt_state);
			}
		}
	}

	tasks->active = false;
	destroy_datapacket_dpdk(pkt_state);

	return (int)ROFL_SUCCESS;
}


//
//Port scheduling
//

/*
* Schedule port. Shedule port to an available core (RR)
*/
rofl_result_t processing_schedule_port(switch_port_t* port){

	unsigned int i, *num_of_ports;
	unsigned int port_id;
	unsigned int lcore_sel, lcore_sel_load = 0xFFFFFFFF;
	unsigned int socket_id, it_load;

	rte_spinlock_lock(&mutex);

	switch(port->type){
		case PORT_TYPE_PHYSICAL:
			if(total_num_of_phy_ports == PROCESSING_MAX_PORTS){
				XDPD_ERR(DRIVER_NAME"[processing] Reached already PROCESSING_MAX_PORTS(%u). All cores are full. No available port slots\n", PROCESSING_MAX_PORTS);
				rte_spinlock_unlock(&mutex);
				return ROFL_FAILURE;
			}
			break;
#ifdef GNU_LINUX_DPDK_ENABLE_NF
		case PORT_TYPE_NF_SHMEM:
		case PORT_TYPE_NF_EXTERNAL:
			if(total_num_of_nf_ports == PROCESSING_MAX_PORTS){
					XDPD_ERR(DRIVER_NAME"[processing] Reached already PROCESSING_MAX_PORTS(%u). All cores are full. No available port slots\n", PROCESSING_MAX_PORTS);
					rte_spinlock_unlock(&mutex);
					return ROFL_FAILURE;
			}
			break;
#endif //GNU_LINUX_DPDK_ENABLE_NF

		default: assert(0);
			return ROFL_FAILURE;
	}

	//Select core
	for(i=0, lcore_sel = RTE_MAX_LCORE; i < RTE_MAX_LCORE; ++i){
		if( processing_core_tasks[i].available &&
			processing_core_tasks[i].num_of_rx_ports != PROCESSING_MAX_PORTS_PER_CORE){

			it_load = processing_core_tasks[i].num_of_rx_ports;

			//For phy ports check for a wrong CPU socket and add additional weight
			if( port->type == PORT_TYPE_PHYSICAL){
				socket_id = rte_eth_dev_socket_id(((dpdk_port_state_t*)port->platform_port_state)->port_id);
				if( (socket_id != 0xFFFFFFFF) && (socket_id != rte_lcore_to_socket_id(i)))
					it_load |= WRONG_CPU_SOCK_OH;
			}

			//Check if this is more appropriate
			if(lcore_sel_load > it_load){
				//Select it
				lcore_sel = i;
				lcore_sel_load = it_load;
			}
		}
	}

	//If they are all full
	if(lcore_sel == RTE_MAX_LCORE){
		XDPD_ERR(DRIVER_NAME"[processing] ERROR: All cores are full. No available port slots\n");
		rte_spinlock_unlock(&mutex);
		return ROFL_FAILURE;
	}

	//Issue a warning if the port is physical and an unmatched CPU socket is being used
	if(port->type == PORT_TYPE_PHYSICAL){
		socket_id = rte_eth_dev_socket_id(((dpdk_port_state_t*)port->platform_port_state)->port_id);
		if( (socket_id != 0xFFFFFFFF) && (socket_id != rte_lcore_to_socket_id(lcore_sel))){
			XDPD_ERR(DRIVER_NAME"[processing] WARNING: The core selected %u[cpu socket %u] and the port %s(cpu socket: %u) are in different CPU sockets!\n This configuration is SUBOPTIMAL!! Consider using another coremask.\n",
				 lcore_sel, rte_lcore_to_socket_id(lcore_sel),
				 port->name, socket_id);
#ifdef ABORT_ON_UNMATCHED_SCHED
			XDPD_ERR(DRIVER_NAME"[processing] ERROR: The core selected %u[cpu socket %u] and the port %s(cpu socket: %u) are in different CPU sockets!\n No available core .\n",
				 lcore_sel, rte_lcore_to_socket_id(lcore_sel),
				 port->name, socket_id);

			rte_spinlock_unlock(&mutex);
			return ROFL_FAILURE;
#endif
		}
	}

	XDPD_DEBUG(DRIVER_NAME"[processing] Selected core %u for scheduling port %s(%p)\n", lcore_sel, port->name, port);

	num_of_ports = &processing_core_tasks[lcore_sel].num_of_rx_ports;

	//Assign port and exit
	if(processing_core_tasks[lcore_sel].port_list[*num_of_ports] != NULL){
		XDPD_ERR(DRIVER_NAME"[processing] Corrupted state on the core task list\n");
		assert(0);
		rte_spinlock_unlock(&mutex);
		return ROFL_FAILURE;
	}

	switch(port->type){
		case PORT_TYPE_PHYSICAL:
		{
			dpdk_port_state_t* port_state = (dpdk_port_state_t*)port->platform_port_state;
			//FIXME: check if already scheduled
			if( iface_manager_set_queues(port, lcore_sel, port_state->port_id) != ROFL_SUCCESS){
				assert(0);
				return ROFL_FAILURE;
			}

			//Store attachment info (back reference)
			port_state->core_id = lcore_sel;
			port_state->core_port_slot = *num_of_ports;


			port_id = port_state->port_id;

			//Increment total counter
			total_num_of_phy_ports++;

			port_state->scheduled = true;
		}
			break;
		case PORT_TYPE_NF_SHMEM:
		{
			dpdk_shmem_port_state_t* port_state = (dpdk_shmem_port_state_t*)port->platform_port_state;

			//Store attachment info (back reference)
			port_state->core_id = lcore_sel;
			port_state->core_port_slot = *num_of_ports;

			port_id = port_state->nf_id;

			//Increment total counter
			total_num_of_nf_ports++;

			port_state->scheduled = true;
		}

			break;
		case PORT_TYPE_NF_EXTERNAL:
		{
			dpdk_kni_port_state_t* port_state = (dpdk_kni_port_state_t*)port->platform_port_state;

			//Store attachment info (back reference)
			port_state->core_id = lcore_sel;
			port_state->core_port_slot = *num_of_ports;

			port_id = port_state->nf_id;

			//Increment total counter
			total_num_of_nf_ports++;

			port_state->scheduled = true;
		}

			break;

		default: assert(0);
			return ROFL_FAILURE;
	}

	processing_core_tasks[lcore_sel].port_list[*num_of_ports] = port;
	(*num_of_ports)++;

	//Mark port as present (and scheduled) on all cores (TX)
	for(i=0;i<RTE_MAX_LCORE;++i){

		switch(port->type){
			case PORT_TYPE_PHYSICAL:
				processing_core_tasks[i].phy_ports[port_id].present = true;
				processing_core_tasks[i].phy_ports[port_id].core_id = lcore_sel;
				break;

#ifdef GNU_LINUX_DPDK_ENABLE_NF
			case PORT_TYPE_NF_SHMEM:
			case PORT_TYPE_NF_EXTERNAL:
				processing_core_tasks[i].nf_ports[port_id].present = true;
				processing_core_tasks[i].nf_ports[port_id].core_id = lcore_sel;
				break;
#endif //GNU_LINUX_DPDK_ENABLE_NF

			default: assert(0);
				return ROFL_FAILURE;
		}
	}


	//Increment the hash counter
	running_hash++;

	rte_spinlock_unlock(&mutex);

	if(!processing_core_tasks[lcore_sel].active){
		if(rte_eal_get_lcore_state(lcore_sel) != WAIT){
			assert(0);
			rte_panic("Core status corrupted!");
		}

		XDPD_DEBUG(DRIVER_NAME"[processing] Launching core %u due to scheduling action of port %p\n", lcore_sel, port);

		//Launch
		XDPD_DEBUG_VERBOSE("Pre-launching core %u due to scheduling action of port %p\n", lcore_sel, port);
		if( rte_eal_remote_launch(processing_core_process_packets, NULL, lcore_sel) < 0)
			rte_panic("Unable to launch core %u! Status was NOT wait (race-condition?)", lcore_sel);
		XDPD_DEBUG_VERBOSE("Post-launching core %u due to scheduling action of port %p\n", lcore_sel, port);
	}

	//Print the status of the cores
	processing_dump_core_states();

	return ROFL_SUCCESS;
}

/*
* Deschedule port to a core
*/
rofl_result_t processing_deschedule_port(switch_port_t* port){

	unsigned int i;
	bool* scheduled;
	unsigned int* core_id, *port_id, *core_port_slot;

	switch(port->type){
		case PORT_TYPE_PHYSICAL:
		{
			dpdk_port_state_t* port_state = (dpdk_port_state_t*)port->platform_port_state;
			scheduled = &port_state->scheduled;
			core_id = &port_state->core_id;
			core_port_slot = &port_state->core_port_slot;
			port_id = &port_state->port_id;
		}
			break;
		case PORT_TYPE_NF_SHMEM:
		{
			dpdk_shmem_port_state_t* port_state = (dpdk_shmem_port_state_t*)port->platform_port_state;
			scheduled = &port_state->scheduled;
			core_id = &port_state->core_id;
			core_port_slot = &port_state->core_port_slot;
			port_id = &port_state->nf_id;

		}
			break;
		case PORT_TYPE_NF_EXTERNAL:
		{
			dpdk_kni_port_state_t* port_state = (dpdk_kni_port_state_t*)port->platform_port_state;

			scheduled = &port_state->scheduled;
			core_id = &port_state->core_id;
			core_port_slot = &port_state->core_port_slot;
			port_id = &port_state->nf_id;

		}

			break;

		default: assert(0);
			return ROFL_FAILURE;
	}

	if(*scheduled == false){
		XDPD_ERR(DRIVER_NAME"[processing] Tyring to descheduled an unscheduled port\n");
		assert(0);
		return ROFL_FAILURE;
	}

	core_tasks_t* core_task = &processing_core_tasks[*core_id];

	rte_spinlock_lock(&mutex);

	//This loop copies from descheduled port, all the rest of the ports
	//one up, so that list of ports is contiguous (0...N-1)
	for(i=*core_port_slot; i<core_task->num_of_rx_ports; i++){
		core_task->port_list[i] = core_task->port_list[i+1];
		if(core_task->port_list[i]){
			switch(core_task->port_list[i]->type){
				case PORT_TYPE_PHYSICAL:
					((dpdk_port_state_t*)core_task->port_list[i]->platform_port_state)->core_port_slot = i;
					break;
				case PORT_TYPE_NF_SHMEM:
					((dpdk_shmem_port_state_t*)core_task->port_list[i]->platform_port_state)->core_port_slot = i;
					break;
				case PORT_TYPE_NF_EXTERNAL:
					((dpdk_kni_port_state_t*)core_task->port_list[i]->platform_port_state)->core_port_slot = i;
					break;
				default: assert(0); //Can never happen
					return ROFL_FAILURE;
			}
		}
	}

	//Decrement counter
	core_task->num_of_rx_ports--;

	//There are no more ports, so simply stop core
	if(core_task->num_of_rx_ports == 0){
		if(rte_eal_get_lcore_state(*core_id) != RUNNING){
			XDPD_ERR(DRIVER_NAME"[processing] Corrupted state; port was marked as active, but EAL informs it was not running..\n");
			assert(0);

		}

		XDPD_DEBUG(DRIVER_NAME"[processing] Shutting down core %u, since port list is empty\n",i);

		core_task->active = false;

		//Wait for core to stop
		rte_eal_wait_lcore(*core_id);
	}

	switch(port->type){
		case PORT_TYPE_PHYSICAL:
			//Decrement total counter
			total_num_of_phy_ports--;
			break;
		case PORT_TYPE_NF_SHMEM:
		case PORT_TYPE_NF_EXTERNAL:
			//Decrement total counter
			total_num_of_nf_ports--;
			break;

		default: assert(0); //Can never happen
			return ROFL_FAILURE;
	}


	//Mark port as NOT present anymore (descheduled) on all cores (TX)
	for(i=0;i<RTE_MAX_LCORE;++i){

		switch(port->type){
			case PORT_TYPE_PHYSICAL:
				processing_core_tasks[i].phy_ports[*port_id].present = false;
				processing_core_tasks[i].phy_ports[*port_id].core_id = 0xFFFFFFFF;
				break;

#ifdef GNU_LINUX_DPDK_ENABLE_NF
			case PORT_TYPE_NF_SHMEM:
			case PORT_TYPE_NF_EXTERNAL:
				processing_core_tasks[i].nf_ports[*port_id].present = false;
				processing_core_tasks[i].nf_ports[*port_id].core_id = 0xFFFFFFFF;
				break;
#endif //GNU_LINUX_DPDK_ENABLE_NF

			default: assert(0);
				return ROFL_FAILURE;
		}
	}

	//Increment the hash counter
	running_hash++;

	//Wait for all the active cores to sync
	processing_wait_for_cores_to_sync();

	rte_spinlock_unlock(&mutex);

	*scheduled = false;

	//Print the status of the cores
	processing_dump_core_states();

	return ROFL_SUCCESS;
}

/*
* Dump core state
*/
void processing_dump_core_states(void){

	unsigned int i,j;
	core_tasks_t* core_task;
	std::stringstream ss;
	enum rte_lcore_role_t role;
	enum rte_lcore_state_t state;

	ss << DRIVER_NAME"[processing] Core status:" << std::endl;

	for(i=0;i<RTE_MAX_LCORE;++i){
		core_task = &processing_core_tasks[i];

		if(i && !core_task->available)
			continue;

		//Print basic info
		ss << "\t core [" << i << "("<<rte_lcore_to_socket_id(i)<<")]";

		if(i == 0){
			ss << " Master"<<std::endl;
			continue;
		}

		role = rte_eal_lcore_role(i);
		state = rte_eal_get_lcore_state(i);

		ss << " role: ";
		switch(role){
			case ROLE_RTE:
				ss << "RTE";
				break;
			case ROLE_OFF:
				ss << "OFF";
				break;
			default:
				assert(0);
				ss << "Unknown";
				break;
		}

		ss << ", state: ";
		switch(state){
			case WAIT:
				ss << "WAIT";
				break;
			case RUNNING:
				ss << "RUNNING";
				break;
			case FINISHED:
				ss << "FINISHED";
				break;
			default:
				assert(0);
				ss << "UNKNOWN";
				break;
		}

		ss << " Load factor: "<< std::fixed << std::setprecision(3) << (float)core_task->num_of_rx_ports/PROCESSING_MAX_PORTS_PER_CORE;
		ss << ", serving ports: [";
		for(j=0;j<core_task->num_of_rx_ports;++j){
			if(core_task->port_list[j] == NULL){
				ss << "error_NULL,";
				continue;
			}
			ss << core_task->port_list[j]->name <<",";
		}
		ss << "]\n";
	}

	XDPD_INFO("%s", ss.str().c_str());
}



