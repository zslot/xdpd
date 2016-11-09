//Guards used only when inlining
#ifndef PACKET_IMPL_INLINE__
#define PACKET_IMPL_INLINE__

#include <rofl_datapath.h>
#include <inttypes.h>
#include <rofl/datapath/pipeline/physical_switch.h>
#include <rofl/datapath/pipeline/common/datapacket.h>
#include <rofl/datapath/pipeline/openflow/of_switch.h>
#include <rofl/datapath/pipeline/platform/packet.h>
#include <rofl/datapath/hal/openflow/openflow1x/of1x_cmm.h>
#include <rofl/datapath/pipeline/common/ipv6_exthdr.h>
#include <utils/c_logger.h>

#include <assert.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "../config.h"
#include <rte_config.h>
#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_spinlock.h>
#include <rte_ring.h>
#include <rte_errno.h>

#include "../io/tx.h"
#include "../io/bufferpool.h"
#include "../io/dpdk_datapacket.h"

//MBUF pool
extern struct rte_mempool* direct_pools[MAX_CPU_SOCKETS];
extern struct rte_mempool* indirect_pools[MAX_CPU_SOCKETS];

/*
* ROFL-Pipeline packet mangling platform API implementation
*/

//
// Configuration to include packet_proto_meta_imp.h
//

#define GET_CLAS_STATE_PTR(PKT)\
 	( &( ( (datapacket_dpdk_t*) PKT ->platform_state) ->clas_state) )

#include "packet_proto_meta_imp.h"

//
// Driver specific functions
//
STATIC_PACKET_INLINE__
void platform_packet_set_queue(datapacket_t* pkt, uint32_t queue)
{
	datapacket_dpdk_t* state = (datapacket_dpdk_t*)pkt->platform_state;
	state->output_queue = queue;	
}

STATIC_PACKET_INLINE__ void
platform_packet_drop(datapacket_t* pkt)
{
	datapacket_dpdk_t* state = (datapacket_dpdk_t*)(pkt->platform_state);
	
	XDPD_DEBUG("Dropping packet(%p)\n",pkt);
	
	if ( NULL == state ){
		XDPD_DEBUG("packet state is NULL\n");
		return;
	}

	if(state->mbuf)	
		rte_pktmbuf_free(state->mbuf);
	
	if( state->packet_in_bufferpool ){
		//Release buffer only if the packet is stored there
		xdpd::gnu_linux::bufferpool::release_buffer(pkt);
	}
	
	return;
}

inline void platform_packet_copy_contents(datapacket_t* pkt, datapacket_t* pkt_copy, struct rte_mbuf* mbuf){

	datapacket_dpdk_t* pkt_dpdk;
	datapacket_dpdk_t* pkt_dpdk_copy;

	//Get the pointers
	pkt_dpdk = (datapacket_dpdk_t*)pkt->platform_state;
	pkt_dpdk_copy = (datapacket_dpdk_t*)pkt_copy->platform_state;
	
	
	//Copy PKT stuff
	memcpy(&pkt_copy->write_actions, &pkt->write_actions ,sizeof(pkt->write_actions));

	//mark as replica
	pkt_copy->is_replica = true;
	pkt_copy->sw = pkt->sw;

	//Initialize the buffer and copy but do not classify
	init_datapacket_dpdk(pkt_dpdk_copy, mbuf, (of_switch_t*)pkt->sw, pkt_dpdk->clas_state.port_in, 0, false, true);

	//Copy output_queue
	pkt_dpdk_copy->output_queue = pkt_dpdk->output_queue;
	//Copy classification state
	pkt_dpdk_copy->clas_state.type = pkt_dpdk->clas_state.type;
	// do not overwrite clas_state.base and clas_state.len, as they are pointing to pkt_dpdk and were set already when calling pkt_dpdk_copy->init(...)
	pkt_dpdk_copy->clas_state.port_in = pkt_dpdk->clas_state.port_in;
	pkt_dpdk_copy->clas_state.phy_port_in = pkt_dpdk->clas_state.phy_port_in;
	pkt_dpdk_copy->clas_state.calculate_checksums_in_sw = pkt_dpdk->clas_state.calculate_checksums_in_sw;
}

/**
* Creates a copy (in heap) of the datapacket_t structure.
* the platform specific state (->platform_state) is copied 
*  depending on the flag copy_mbuf
*/

//Disable soft cloning
#define DISABLE_SOFT_CLONE

STATIC_PACKET_INLINE__ datapacket_t* platform_packet_replicate__(datapacket_t* pkt, bool hard_clone){

	datapacket_t* pkt_replica;
	struct rte_mbuf* mbuf=NULL, *mbuf_origin;
	
	//Protect
	if(unlikely(!pkt))
		return NULL;

	//datapacket_t* pkt_replica;
	pkt_replica = xdpd::gnu_linux::bufferpool::get_buffer();
	
	if(unlikely(!pkt_replica)){
		XDPD_DEBUG("Replicate packet; could not clone pkt(%p). No buffers left in bufferpool\n", pkt);
		goto PKT_REPLICATE_ERROR;
	}

	mbuf_origin = ((datapacket_dpdk_t*)pkt->platform_state)->mbuf;	

#ifndef DISABLE_SOFT_CLONE
	if( hard_clone ){
#endif
		mbuf = rte_pktmbuf_alloc(direct_pools[rte_socket_id()]);
		
		if(unlikely(mbuf == NULL)){	
			XDPD_DEBUG("Replicate packet; could not hard clone pkt(%p). rte_pktmbuf_clone failed. errno: %d - %s\n", pkt_replica, rte_errno, rte_strerror(rte_errno));
			goto PKT_REPLICATE_ERROR;
		}
		if(unlikely( rte_pktmbuf_append(mbuf, rte_pktmbuf_pkt_len(mbuf_origin)) == NULL)){
			XDPD_DEBUG("Replicate packet(hard); could not perform rte_pktmbuf_append pkt(%p). rte_pktmbuf_clone failed\n", pkt_replica);
			goto PKT_REPLICATE_ERROR;
		}
		rte_memcpy(rte_pktmbuf_mtod(mbuf, uint8_t*), rte_pktmbuf_mtod(mbuf_origin, uint8_t*),  rte_pktmbuf_pkt_len(mbuf_origin));
		assert( rte_pktmbuf_pkt_len(mbuf) == rte_pktmbuf_pkt_len(mbuf_origin) );

#ifndef DISABLE_SOFT_CLONE
	} else {
		//Soft clone
		mbuf = rte_pktmbuf_clone(mbuf_origin, pool_indirect);
		
		if(unlikely(mbuf == NULL)){	
			XDPD_DEBUG("Replicate packet; could not hard clone pkt(%p). rte_pktmbuf_clone failed\n", pkt);
			goto PKT_REPLICATE_ERROR;
		}
	}
#endif

	//Copy datapacket_t and datapacket_dpdk_t state
	platform_packet_copy_contents(pkt, pkt_replica, mbuf);

	return pkt_replica; //DO NOT REMOVE

PKT_REPLICATE_ERROR:
	assert(0); 
	
	//Release packet
	if(pkt_replica){
		xdpd::gnu_linux::bufferpool::release_buffer(pkt_replica);

		if(mbuf){
			rte_pktmbuf_free(mbuf);
		}
	}

	return NULL;
}

/*
* Detaches mbuf from stack allocated pkt and copies the content
*/
STATIC_PACKET_INLINE__ 
datapacket_t* platform_packet_detach__(datapacket_t* pkt){

	struct rte_mbuf* mbuf_origin;
	datapacket_t* pkt_detached = xdpd::gnu_linux::bufferpool::get_buffer();

	if(unlikely( pkt_detached == NULL))
		return NULL;
	
	mbuf_origin = ((datapacket_dpdk_t*)pkt->platform_state)->mbuf;	

	//Copy the contents
	platform_packet_copy_contents(pkt, pkt_detached, mbuf_origin);

	//Really detach the mbuf from the original instance
	((datapacket_dpdk_t*)pkt->platform_state)->mbuf = NULL;
	
	return pkt_detached;
}

/**
* Creates a copy (in heap) of the datapacket_t structure including any
* platform specific state (->platform_state). The following behaviour
* is expected from this hook:
* 
* - All data fields and pointers of datapacket_t struct must be memseted to 0, except:
* - datapacket_t flag is_replica must be set to true
* - platform_state, if used, must be replicated (copied) otherwise NULL
*
*/
STATIC_PACKET_INLINE__ datapacket_t* platform_packet_replicate(datapacket_t* pkt){
	return platform_packet_replicate__(pkt, true);
}


static inline void output_single_packet(datapacket_t* pkt, datapacket_dpdk_t* pack, switch_port_t* port){

	//Output packet to the appropiate queue and port_num
	if(likely(port && port->platform_port_state) && port->up && port->forward_packets){
		
		XDPD_DEBUG("[%s] OUTPUT packet(%p)\n", port->name, pkt);

		if(port->type == PORT_TYPE_VIRTUAL){
			/*
			* Virtual link
			*/
			//Reset port_in and reprocess
			((datapacket_dpdk_t*)pkt->platform_state)->clas_state.port_in =  ((switch_port_t*)port->platform_port_state)->of_port_num;
	
			xdpd::gnu_linux_dpdk::tx_pkt_vlink(port, pkt);
			return;
		}
#ifdef GNU_LINUX_DPDK_ENABLE_NF		
		else if(port->type == PORT_TYPE_NF_SHMEM)
		{
			/*
			* DPDK NF port
			*/
			xdpd::gnu_linux_dpdk::tx_pkt_shmem_nf_port(port, pkt);
		}else if(port->type == PORT_TYPE_NF_EXTERNAL)
		{
			/*
			* KNI NF port
			*/
			xdpd::gnu_linux_dpdk::tx_pkt_kni_nf_port(port, pkt);
		}
#endif		
		else{
			xdpd::gnu_linux_dpdk::tx_pkt(port, pack->output_queue, pkt);
		}
	}else{
		//Since tx_pkt is not called, we release the mbuf here
		//pkt will be returned only in case it is in_bufferpool
		rte_pktmbuf_free(((datapacket_dpdk_t*)pkt->platform_state)->mbuf);
	}

	if( ((datapacket_dpdk_t*)pkt->platform_state)->packet_in_bufferpool ){
		//Release buffer only if the packet is stored there
		xdpd::gnu_linux::bufferpool::release_buffer(pkt);
	}
}

/**
* Output packet to the port(s)
* The action HAS to implement the destruction/release of the pkt
* (including if the pkt is a replica).
*
* If a flooding output actions needs to be done, the function
* has itself to deal with packet replication.
*/
STATIC_PACKET_INLINE__ void platform_packet_output(datapacket_t* pkt, switch_port_t* output_port){

	of_switch_t const* sw;
	datapacket_dpdk_t* pack;

	if(!output_port){
		assert(0);
		return;
	}
	
	//Check whether dpx86 is NULL
	pack = (datapacket_dpdk_t*) (pkt->platform_state);
	assert(pack != NULL);

	//Recalculate checksums
	calculate_checksums_in_software(pkt);

	//flood_meta_port is a static variable defined in the physical_switch
	//the meta_port
	if(output_port == flood_meta_port || output_port == all_meta_port){ //We don't have STP, so it is the same
		datapacket_t* replica;
		switch_port_t* port_it;

		//Get switch
		sw = pkt->sw;	
#ifdef DEBUG
		if(unlikely(!sw)){
			// NOTE release here mbuf as well?
			rte_pktmbuf_free(((datapacket_dpdk_t*)pkt->platform_state)->mbuf);
			xdpd::gnu_linux::bufferpool::release_buffer(pkt);
			return;
		}
#endif
	
		//We need to flood
		for(unsigned i=0;i<LOGICAL_SWITCH_MAX_LOG_PORTS;++i){

			port_it = sw->logical_ports[i].port;

			//Check port is not incomming port, exists, and is up 
			if( (i == pack->clas_state.port_in) || !port_it || port_it->no_flood)
				continue;

			//replicate packet
			replica = platform_packet_replicate__(pkt, false);

			if(unlikely(!replica)){
				XDPD_ERR("ERROR: Could not complete FLOOD action(%p): out of memory!\n", pkt);
				assert(0);
				break;
			}

			XDPD_DEBUG("[%s] OUTPUT FLOOD packet(%p), origin(%p)\n", port_it->name, replica, pkt);
			
			//send the replica
			output_single_packet(replica, (datapacket_dpdk_t*)replica->platform_state, port_it);
		}
			
		//discard the original packet always (has been replicated)
		rte_pktmbuf_free(((datapacket_dpdk_t*)pkt->platform_state)->mbuf);
		if( ((datapacket_dpdk_t*)pkt->platform_state)->packet_in_bufferpool )
			xdpd::gnu_linux::bufferpool::release_buffer(pkt);
	}else if(output_port == in_port_meta_port){
		
		//In port
		switch_port_t* port;
		sw = pkt->sw;	

		if(unlikely(pack->clas_state.port_in >= LOGICAL_SWITCH_MAX_LOG_PORTS)){
			assert(0);
			return;
		}

		port = sw->logical_ports[pack->clas_state.port_in].port;
		if( unlikely(port == NULL)){
			assert(0);
			return;
		
		}
	
		//Send to the incomming port 
		output_single_packet(pkt, pack, port);
	}else{
		//Single output	
		output_single_packet(pkt, pack, output_port);
	}

}

#endif //Guards
