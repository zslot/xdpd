#include "ioport_mmap.h"
#include <sched.h>
#include "../../bufferpool.h"
#include "../../datapacketx86.h"
#include "../../../util/likely.h"
#include "../../iomanager.h"

#include <linux/ethtool.h>
#if 0
#include <rofl/common/protocols/fetherframe.h>
#include <rofl/common/protocols/fvlanframe.h>
#endif

//Profiling
#include "../../../util/time_measurements.h"
#include "../../../config.h"
#include "../../../c_logger.h"

using namespace rofl;
using namespace xdpd::gnu_linux;

//Constructor and destructor
ioport_mmap::ioport_mmap(
		/*int port_no,*/
		switch_port_t* of_ps,
		int block_size,
		int n_blocks,
		int frame_size,
		unsigned int num_queues) :
			ioport(of_ps, num_queues),
			rx(NULL),
			tx(NULL),
			block_size(block_size),
			n_blocks(n_blocks),
			frame_size(frame_size),
			deferred_drain(0)
{
	int rc;
	
	//Open pipe for output signaling on enqueue	
	rc = pipe(notify_pipe);
	(void)rc; // todo use the value

	//Set non-blocking read/write in the pipe
	for(unsigned int i=0;i<2;i++){
		int flags = fcntl(notify_pipe[i], F_GETFL, 0);	///get current file status flags
		flags |= O_NONBLOCK;				//turn off blocking flag
		fcntl(notify_pipe[i], F_SETFL, flags);		//set up non-blocking read
	}


}


ioport_mmap::~ioport_mmap()
{
	if(rx)
		delete rx;
	if(tx)
		delete tx;

	close(notify_pipe[READ]);
	close(notify_pipe[WRITE]);
}

//Read and write methods over port
void ioport_mmap::enqueue_packet(datapacket_t* pkt, unsigned int q_id){

	const char c='a';
	int ret;
	unsigned int len;
	
	datapacketx86* pkt_x86 = (datapacketx86*) pkt->platform_state;
	len = pkt_x86->get_buffer_length();

	if ( likely(of_port_state->up) && 
		likely(of_port_state->forward_packets) &&
		likely(len >= MIN_PKT_LEN) ) {

		//Safe check for q_id
		if( unlikely(q_id >= get_num_of_queues()) ){
			XDPD_DEBUG(DRIVER_NAME"[mmap:%s] Packet(%p) trying to be enqueued in an invalid q_id: %u\n",  of_port_state->name, pkt, q_id);
			q_id = 0;
			bufferpool::release_buffer(pkt);
			assert(0);
		}
	
		//Store on queue and exit. This is NOT copying it to the mmap buffer
		if(output_queues[q_id]->non_blocking_write(pkt) != ROFL_SUCCESS){
			TM_STAMP_STAGE(pkt, TM_SA5_FAILURE);
			
			XDPD_DEBUG(DRIVER_NAME"[mmap:%s] Packet(%p) dropped. Congestion in output queue: %d\n",  of_port_state->name, pkt, q_id);
			//Drop packet
			bufferpool::release_buffer(pkt);

#ifndef IO_KERN_DONOT_CHANGE_SCHED
			//Force descheduling (prioritize TX)
			sched_yield();	
#endif
			return;
		}
		TM_STAMP_STAGE(pkt, TM_SA5_SUCCESS);

		XDPD_DEBUG_VERBOSE(DRIVER_NAME"[mmap:%s] Packet(%p) enqueued, buffer size: %d\n",  of_port_state->name, pkt, output_queues[q_id]->size());
	
		//WRITE to pipe
		ret = ::write(notify_pipe[WRITE],&c,sizeof(c));
		(void)ret; // todo use the value
	} else {
		if(len < MIN_PKT_LEN){
			XDPD_ERR(DRIVER_NAME"[mmap:%s] ERROR: attempt to send invalid packet size for packet(%p) scheduled for queue %u. Packet size: %u\n", of_port_state->name, pkt, q_id, len);
			assert(0);
		}else{
			XDPD_DEBUG_VERBOSE(DRIVER_NAME"[mmap:%s] dropped packet(%p) scheduled for queue %u\n", of_port_state->name, pkt, q_id);
		}

		//Drop packet
		bufferpool::release_buffer(pkt);
	}

}

inline void ioport_mmap::empty_pipe(){
	int ret;

	if(unlikely(deferred_drain == 0))
		return;

	//Just take deferred_drain from the pipe 
	if(deferred_drain > IO_IFACE_RING_SLOTS)	
		ret = ::read(notify_pipe[READ], draining_buffer, IO_IFACE_RING_SLOTS);
	else
		ret = ::read(notify_pipe[READ], draining_buffer, deferred_drain);
	

	if(ret > 0){
		deferred_drain -= ret;
		
		if(unlikely( deferred_drain< 0 ) ){
			assert(0); //Desynchronized
			deferred_drain = 0;
		}
	}
}

inline void ioport_mmap::fill_vlan_pkt(struct tpacket2_hdr *hdr, datapacketx86 *pkt_x86){

	//Initialize pktx86
	pkt_x86->init(NULL, hdr->tp_len + sizeof(struct vlan_hdr_t), of_port_state->attached_sw, get_port_no(), 0, false); //Init but don't classify

	// write ethernet header
	memcpy(pkt_x86->get_buffer(), (uint8_t*)hdr + hdr->tp_mac, sizeof(struct eth_hdr_t));

#ifndef KERNEL_STAG_SUPPORT
	// set dl_type to vlan
	if( htobe16(ETH_P_8021Q) == ((struct eth_hdr_t*)((uint8_t*)hdr + hdr->tp_mac))->dl_type ) {
		((struct eth_hdr_t*)pkt_x86->get_buffer())->dl_type = htobe16(ETH_P_8021Q); // tdoo maybe this should be ETH_P_8021AD
	}else{
		((struct eth_hdr_t*)pkt_x86->get_buffer())->dl_type = htobe16(ETH_P_8021Q);
	}
#endif

	// write vlan
	struct vlan_hdr_t* vlanptr =
			(struct vlan_hdr_t*) (pkt_x86->get_buffer()
			+ sizeof(struct eth_hdr_t));
	vlanptr->byte0 =  (hdr->tp_vlan_tci >> 8);
	vlanptr->byte1 = hdr->tp_vlan_tci & 0x00ff;
	vlanptr->dl_type = ((struct eth_hdr_t*)((uint8_t*)hdr + hdr->tp_mac))->dl_type;

#ifdef KERNEL_STAG_SUPPORT
	// set dl_type to C-TAG, S-TAG, I-TAG, as indicated by kernel
	((struct eth_hdr_t*)pkt_x86->get_buffer())->dl_type = htobe16(hdr->tp_vlan_tpid);
#endif

	// write payload
	memcpy(pkt_x86->get_buffer() + sizeof(struct eth_hdr_t) + sizeof(struct vlan_hdr_t),
	(uint8_t*)hdr + hdr->tp_mac + sizeof(struct eth_hdr_t),
	hdr->tp_len - sizeof(struct eth_hdr_t));

	//And classify
	classify_packet(&pkt_x86->clas_state, pkt_x86->get_buffer(), pkt_x86->get_buffer_length(), get_port_no(), 0);
}
	
// handle read
datapacket_t* ioport_mmap::read(){

	struct tpacket2_hdr *hdr;
	struct sockaddr_ll *sll;
	datapacket_t *pkt;
	datapacketx86 *pkt_x86;
	uint8_t* pkt_mac;

	//Check if we really have to read
	if(!of_port_state->up || of_port_state->drop_received || !rx)
		return NULL;

next:
	//Retrieve a packet	
 	hdr = rx->read_packet();

	//No packets available
	if (!hdr)
		return NULL;

	//Sanity check 
	if ( unlikely(hdr->tp_mac + hdr->tp_snaplen > rx->get_tpacket_req()->tp_frame_size) ) {
		XDPD_DEBUG_VERBOSE(DRIVER_NAME"[mmap:%s] sanity check during read mmap failed\n",of_port_state->name);
		//Increment error statistics
		of_port_state->stats.rx_dropped++;		

		//Return packet to kernel in the RX ring		
		rx->return_packet(hdr);
		return NULL;
	}

	//Check if it is an ongoing frame from TX
	sll = (struct sockaddr_ll*)((uint8_t*)hdr + TPACKET_ALIGN(sizeof(struct tpacket_hdr)));
	if (PACKET_OUTGOING == sll->sll_pkttype) {
		/*XDPD_DEBUG_VERBOSE(DRIVER_NAME" cioport(%s)::handle_revent() outgoing "
					"frame rcvd in slot i:%d, ignoring\n", of_port_state->name, rx->rpos);*/

		//Return packet to kernel in the RX ring		
		rx->return_packet(hdr);
		goto next;
	}
	
	//Discard frames generated by the switch or the OS (feedback)
	pkt_mac = ((struct eth_hdr_t*)((uint8_t*)hdr + hdr->tp_mac))->dl_src;
	if (memcmp(pkt_mac, mac, ETHER_MAC_LEN) == 0 ){
		/*XDPD_DEBUG_VERBOSE(DRIVER_NAME" cioport(%s)::handle_revent() outgoing "
		"frame rcvd in slot i:%d, src-mac == own-mac, ignoring\n", of_port_state->name, rx->rpos);*/

		//Return packet to kernel in the RX ring		
		rx->return_packet(hdr);
		goto next;
	}

	//Retrieve buffer from pool: this is a non-blocking call
	pkt = bufferpool::get_buffer();

	//Handle no free buffer
	if(!pkt) {
		//Increment error statistics and drop
		of_port_state->stats.rx_dropped++;		
		rx->return_packet(hdr);
		return NULL;
	}
			
	pkt_x86 = (datapacketx86*) pkt->platform_state;

	//Fill packet
	#ifdef TP_STATUS_VLAN_VALID
	if(hdr->tp_status&TP_STATUS_VLAN_VALID){
	#else
	if(hdr->tp_vlan_tci != 0) {
        #endif			
		//There is a VLAN
		fill_vlan_pkt(hdr, pkt_x86);	
	}else{
		// no vlan tag present
		pkt_x86->init((uint8_t*)hdr + hdr->tp_mac, hdr->tp_len, of_port_state->attached_sw, get_port_no(), 0);
	}

	//Timestamp S2	
	TM_STAMP_STAGE(pkt, TM_S2);
	classify_packet(&pkt_x86->clas_state, pkt_x86->get_buffer(), pkt_x86->get_buffer_length(), get_port_no(), 0);

	//Return packet to kernel in the RX ring		
	rx->return_packet(hdr);

	//Increment statistics&return
	of_port_state->stats.rx_packets++;
	of_port_state->stats.rx_bytes += pkt_x86->get_buffer_length();
	
	return pkt;


}

inline void ioport_mmap::fill_tx_slot(struct tpacket2_hdr *hdr, datapacketx86 *packet){

	uint8_t *data = ((uint8_t *) hdr) + TPACKET2_HDRLEN - sizeof(struct sockaddr_ll);
	memcpy(data, packet->get_buffer(), packet->get_buffer_length());

#if 0
	XDPD_DEBUG_VERBOSE(DRIVER_NAME" %s(): datapacketx86 %p to tpacket_hdr %p\n"
			"	data = %p\n,"
			"	with content:\n", __FUNCTION__, packet, hdr, data);
	packet->dump();
#endif
	hdr->tp_len = packet->get_buffer_length();
	hdr->tp_snaplen = packet->get_buffer_length();
	hdr->tp_status = TP_STATUS_SEND_REQUEST;

}

unsigned int ioport_mmap::write(unsigned int q_id, unsigned int num_of_buckets){

	struct tpacket2_hdr *hdr;
	datapacket_t* pkt;
	datapacketx86* pkt_x86;
	unsigned int cnt = 0;
	int tx_bytes_local = 0;

	circular_queue<datapacket_t>* queue = output_queues[q_id];

	if ( unlikely(tx == NULL) ) {
		return num_of_buckets;
	}

	// read available packets from incoming buffer
	for ( ; 0 < num_of_buckets; --num_of_buckets ) {

		
		//Check
		if(queue->size() == 0){
			XDPD_DEBUG_VERBOSE(DRIVER_NAME"[mmap:%s] no packet left in output_queue %u left, %u buckets left\n",
					of_port_state->name,
					q_id,
					num_of_buckets);
			break;
		}

		//Retrieve an empty slot in the TX ring
		hdr = tx->get_free_slot();

		//Skip, TX is full
		if(!hdr)
			break;
		
		//Retrieve the buffer
		pkt = queue->non_blocking_read();
		
		if(!pkt){
			XDPD_ERR(DRIVER_NAME"[mmap:%s] A packet has been discarded due to race condition on the output queue. Are you really running the TX group with a single thread? output_queue %u left, %u buckets left\n",
				of_port_state->name,
				q_id,
				num_of_buckets);
		
			assert(0);
			break;
		}
	
		TM_STAMP_STAGE(pkt, TM_SA6);
		
		pkt_x86 = (datapacketx86*) pkt->platform_state;

		if(unlikely(pkt_x86->get_buffer_length() > mps)){
			//This should NEVER happen
			XDPD_ERR(DRIVER_NAME"[mmap:%s] Packet length above the Max Packet Size (MPS). Packet length: %u, MPS %u.. discarding\n", of_port_state->name, pkt_x86->get_buffer_length(), mps);
			assert(0);
		
			//Return buffer to the pool
			bufferpool::release_buffer(pkt);
		
			//Increment errors
			of_port_state->queues[q_id].stats.overrun++;
			of_port_state->stats.tx_dropped++;
			
			deferred_drain++;
			continue;
		}else{	
			fill_tx_slot(hdr, pkt_x86);
		}
		
		TM_STAMP_STAGE(pkt, TM_SA7);
		
		//Return buffer to the pool
		bufferpool::release_buffer(pkt);


		tx_bytes_local += hdr->tp_len;
		cnt++;
		deferred_drain++;
	}
	
	//Increment stats and return
	if (likely(cnt > 0)) {
		XDPD_DEBUG_VERBOSE(DRIVER_NAME"[mmap:%s] schedule %u packet(s) to be send\n", __FUNCTION__, cnt);

		// send packets in TX
		if(unlikely(tx->send() != ROFL_SUCCESS)){
			XDPD_ERR(DRIVER_NAME"[mmap:%s] ERROR while sending packets. This is due very likely to an invalid ETH_TYPE value. Now the port will be reset in order to continue operation\n", of_port_state->name);
			assert(0);
			of_port_state->stats.tx_errors += cnt;
			of_port_state->queues[q_id].stats.overrun += cnt;
			

			/*
			* We need to reset the port, meaning destroy and regenerate both TX rings
			* Disabling and enabling the port to accomplish so.
			*/
			if(tx){
				delete tx;
				tx = new mmap_tx(std::string(of_port_state->name), block_size, n_blocks, frame_size); 
			}	
			
			
			//Making sure fds are regenerated by manually incrementing pg hash	
			portgroup_state* pg = iomanager::get_group(iomanager::get_group_id_by_port((ioport*)this, PG_TX));
			if(!pg){
				assert(0);
				XDPD_DEBUG(DRIVER_NAME"[mmap:%s] ERROR: Unable to update port-group hash. The port might be left unusable\n", of_port_state->name);
			}else
				XDPD_DEBUG(DRIVER_NAME"[mmap:%s] Port reset was successful\n", of_port_state->name);
			pg->running_hash++;
					
		}

		//Increment statistics
		of_port_state->stats.tx_packets += cnt;
		of_port_state->stats.tx_bytes += tx_bytes_local;
		of_port_state->queues[q_id].stats.tx_packets += cnt;
		of_port_state->queues[q_id].stats.tx_bytes += tx_bytes_local;
		
	}

	//Empty reading pipe (batch)
	empty_pipe();

	// return not used buckets
	return num_of_buckets;
}

/*
 * Disable tx checksum offload in an interface given its name
 */
void disable_iface_checksum_offloading(int sd, struct ifreq ifr){
	struct ethtool_value eval;
	eval.cmd = ETHTOOL_GTXCSUM;
	ifr.ifr_data = (caddr_t)&eval;
	eval.data = 0;//Make valgrind happy

	if (ioctl(sd, SIOCETHTOOL, &ifr) < 0) {
		XDPD_WARN(DRIVER_NAME"[mmap:%s] Unable to detect if the Tx Checksum Offload feature on the NIC is enabled or not. Please make sure it is disabled using ethtool or similar...\n", ifr.ifr_name);
	} else {
		if (eval.data == 0) {
			//Show nice messages in debug mode
			XDPD_DEBUG(DRIVER_NAME"[mmap:%s] Tx Checksum Offload already disabled.\n", ifr.ifr_name);
		} else {
			//Do it
			eval.cmd = ETHTOOL_STXCSUM;
			eval.data = 0;
			ifr.ifr_data = (caddr_t)&eval;

			if (ioctl(sd, SIOCETHTOOL, &ifr) < 0)
				XDPD_ERR(DRIVER_NAME"[mmap:%s] Could not disable Tx Checksum Offload feature on the NIC. This can be potentially dangeros...be advised!\n",  ifr.ifr_name);
			else
				XDPD_DEBUG(DRIVER_NAME"[mmap:%s] Tx Checksum Offload successfully disabled.\n", ifr.ifr_name);
		}
	}
}

/*
 * For veth intefaces we can disable the TX checksum
 * offload to make sure that the kernel is calculating
 * the checksum
 */
int check_veth_interface(int sd, struct ifreq ifr){
	
	int peer_id=0;
	struct ethtool_drvinfo drvinfo;
	struct ethtool_gstrings *strings = NULL;
	struct ethtool_stats* stats = NULL;
	
	// Get driver info (to find out the number of stats)
	memset(&drvinfo, 0, sizeof(struct ethtool_drvinfo));
	drvinfo.cmd = ETHTOOL_GDRVINFO;
	ifr.ifr_data = (caddr_t) &drvinfo;
	if (ioctl(sd, SIOCETHTOOL, &ifr) < 0) {
		XDPD_WARN(DRIVER_NAME"[mmap:%s] Unable to get driver info for interface\n", ifr.ifr_name);
		return -1;
	}

	if (drvinfo.n_stats>0){
		// Create the structures that will hold the statistics (each statistic name has a length of ETH_GSTRING_LEN)
		
		strings = (struct ethtool_gstrings *) calloc(1, drvinfo.n_stats*ETH_GSTRING_LEN + sizeof(struct ethtool_gstrings));
		stats = (struct ethtool_stats*) calloc(1, drvinfo.n_stats*sizeof(uint64_t) + sizeof(struct ethtool_stats));
		if (!strings || !stats){
			XDPD_ERR(DRIVER_NAME"Error allocaing memory\n");
			return -1;
		}

		// Request stats names
		strings->cmd = ETHTOOL_GSTRINGS;
		strings->string_set = ETH_SS_STATS;
		strings->len = drvinfo.n_stats;
		ifr.ifr_data = (caddr_t) strings;
		if (ioctl(sd, SIOCETHTOOL, &ifr) < 0) {
			XDPD_WARN(DRIVER_NAME"[mmap:%s] Unable to get statistics name vector for interface\n", ifr.ifr_name);
			free(strings);
			free(stats);
			return -1;
		}

		// Request stats values
		stats->cmd = ETHTOOL_GSTATS;
		stats->n_stats = drvinfo.n_stats;
		ifr.ifr_data = (caddr_t)stats;
		if (ioctl(sd, SIOCETHTOOL, &ifr) < 0) {
			XDPD_WARN(DRIVER_NAME"[mmap:%s] Unable to get ethtool stats feature on the NIC. Tx Checksum Offload feature won't be checked\n", ifr.ifr_name);
			free(strings);
			free(stats);
			return -1;
		}

		// Look for peer_ifindex
		uint16_t i;
		char peer_id_str[] = "peer_ifindex";
		for(i=0; i<drvinfo.n_stats; i++){
			if( strncmp(peer_id_str, (char *)&strings->data[i * ETH_GSTRING_LEN], sizeof(peer_id_str)) == 0 ){
				//XDPD_DEBUG(DRIVER_NAME"[mmap:%s] Found %s %llu\n", ifr.ifr_name, (char *)&strings->data[i * ETH_GSTRING_LEN], stats->data[i]);
				break;
			}
		}
		if(i == drvinfo.n_stats){
			XDPD_DEBUG(DRIVER_NAME"[mmap:%s] No veth peer detected\n", ifr.ifr_name);
			peer_id = 0;
		}else{
			// Interface is a VETH type. peer is stats->data[i]
			peer_id = stats->data[i];
		}

		free(strings);
		free(stats);
		return peer_id;
	
	}else{
		XDPD_WARN(DRIVER_NAME"[mmap:%s] No statistics found in interface\n", ifr.ifr_name);
	}

	return 0;
}

/*
*
* Enable and down port routines
*
*/
rofl_result_t ioport_mmap::up() {
	
	struct ifreq ifr;
	int sd, rc;
	struct ethtool_value eval;

	XDPD_DEBUG(DRIVER_NAME"[mmap:%s] Trying to bring up\n",of_port_state->name);
	
	if ((sd = socket(AF_PACKET, SOCK_RAW, 0)) < 0){
		return ROFL_FAILURE;
	}

	memset(&ifr, 0, sizeof(struct ifreq));
	strcpy(ifr.ifr_name, of_port_state->name);

	if ((rc = ioctl(sd, SIOCGIFINDEX, &ifr)) < 0){
		return ROFL_FAILURE;
	}

	/*
	* Make sure we are disabling Generic and Large Receive Offload from the NIC.
	* This screws up the MMAP
	*/

	//First retrieve the current gro setup, so that we can gently
	//inform the user we are going to disable (and not set it back)
	eval.cmd = ETHTOOL_GGRO;
	ifr.ifr_data = (caddr_t)&eval;
	eval.data = 0;//Make valgrind happy

	if (ioctl(sd, SIOCETHTOOL, &ifr) < 0) {
		XDPD_WARN(DRIVER_NAME"[mmap:%s] Unable to detect if the Generic Receive Offload (GRO) feature on the NIC is enabled or not. Please make sure it is disabled using ethtool or similar...\n", of_port_state->name);
		
	}else{
		//Show nice messages in debug mode
		if(eval.data == 0){
			XDPD_DEBUG(DRIVER_NAME"[mmap:%s] GRO already disabled.\n", of_port_state->name);
		}else{
			//Do it
			eval.cmd = ETHTOOL_SGRO;
			eval.data = 0;
			ifr.ifr_data = (caddr_t)&eval;
			
			if (ioctl(sd, SIOCETHTOOL, &ifr) < 0) {
				XDPD_ERR(DRIVER_NAME"[mmap:%s] Could not disable Generic Receive Offload feature on the NIC. This can be potentially dangeros...be advised!\n",  of_port_state->name);
			}else{
				XDPD_DEBUG(DRIVER_NAME"[mmap:%s] GRO successfully disabled.\n", of_port_state->name);
			}

		}
	}

	//Now LRO
	eval.cmd = ETHTOOL_GFLAGS;
	ifr.ifr_data = (caddr_t)&eval;
	eval.data = 0;//Make valgrind happy

	if (ioctl(sd, SIOCETHTOOL, &ifr) < 0) {
		XDPD_WARN(DRIVER_NAME"[mmap:%s] Unable to detect if the Large Receive Offload (LRO) feature on the NIC is enabled or not. Please make sure it is disabled using ethtool or similar...\n", of_port_state->name);
	} else {
		if ((eval.data & ETH_FLAG_LRO) == 0) {
			//Show nice messages in debug mode
			XDPD_DEBUG(DRIVER_NAME"[mmap:%s] LRO already disabled.\n", of_port_state->name);
		} else {
			//Do it
			eval.cmd = ETHTOOL_SFLAGS;
			eval.data = (eval.data & ~ETH_FLAG_LRO);
			ifr.ifr_data = (caddr_t)&eval;

			if (ioctl(sd, SIOCETHTOOL, &ifr) < 0)
				XDPD_ERR(DRIVER_NAME"[mmap:%s] Could not disable Large Receive Offload (LRO) feature on the NIC. This can be potentially dangeros...be advised!\n",  of_port_state->name);
			else
				XDPD_DEBUG(DRIVER_NAME"[mmap:%s] LRO successfully disabled.\n", of_port_state->name);
		}
	}

#if VETH_DISABLE_CHKSM_OFFLOAD
	// Checksum Offload
	int peer_id;
	if ( (peer_id=check_veth_interface(sd, ifr)) > 0 ){
		struct ifreq peer_ifr;
		int peer_sd, peer_rv;
		if ((peer_sd = socket(AF_PACKET, SOCK_RAW, 0)) < 0){
			return ROFL_FAILURE;
		}

		memset(&peer_ifr, 0, sizeof(struct ifreq));
		peer_ifr.ifr_ifindex = peer_id;

		if ((peer_rv = ioctl(peer_sd, SIOCGIFNAME, &peer_ifr)) < 0){
			close(peer_sd);
			return ROFL_FAILURE;
		}
		XDPD_DEBUG(DRIVER_NAME"[mmap:%s] Veth iface detected: peer id = %llu : name %s\n", ifr.ifr_name, peer_id, peer_ifr.ifr_name);
		
		//Disable chk offload in both the interface and the link
		disable_iface_checksum_offloading(peer_sd, peer_ifr);
		disable_iface_checksum_offloading(sd, ifr);
		close(peer_sd);
	}
#endif	
	
	//Recover MTU
	memset((void*)&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, of_port_state->name, sizeof(ifr.ifr_name));
	
	if(ioctl(sd, SIOCGIFMTU, &ifr) < 0) {
		XDPD_ERR(DRIVER_NAME"[mmap:%s] Could not retreive MTU value from NIC. Default %u Max Packet Size(MPS) size will be used (%u total bytes). Packets exceeding this size will be DROPPED (Jumbo frames).\n",  of_port_state->name, (PORT_DEFAULT_PKT_SIZE-PORT_ETHER_LENGTH), PORT_DEFAULT_PKT_SIZE);
		mps = PORT_DEFAULT_PKT_SIZE;	
	}else{
		mps = ifr.ifr_mtu+PORT_ETHER_LENGTH;
		XDPD_DEBUG(DRIVER_NAME"[mmap:%s] Discovered Max Packet Size(MPS) of %u.\n",  of_port_state->name, mps);
	}

	//Recover flags
	if ((rc = ioctl(sd, SIOCGIFFLAGS, &ifr)) < 0){ 
		close(sd);
		return ROFL_FAILURE;
	}

	// enable promiscous mode
	memset((void*)&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, of_port_state->name, sizeof(ifr.ifr_name));
	
	if ((rc = ioctl(sd, SIOCGIFFLAGS, &ifr)) < 0){
		close(sd);
		return ROFL_FAILURE;
	}

	ifr.ifr_flags |= IFF_PROMISC;
	if ((rc = ioctl(sd, SIOCSIFFLAGS, &ifr)) < 0){
		close(sd);
		return ROFL_FAILURE;
	}
	
	//Check if is up or not
	if (IFF_UP & ifr.ifr_flags){
		
		//Already up.. Silently skip
		close(sd);

		//If tx/rx lines are not created create them
		if(!rx){	
			XDPD_DEBUG_VERBOSE(DRIVER_NAME"[mmap:%s] generating a new mmap_rx for RX\n",of_port_state->name);
			rx = new mmap_rx(std::string(of_port_state->name), 2 * block_size, n_blocks, frame_size);
		}
		if(!tx){
			XDPD_DEBUG_VERBOSE(DRIVER_NAME"[mmap:%s] generating a new mmap_tx for TX\n",of_port_state->name);
			tx = new mmap_tx(std::string(of_port_state->name), block_size, n_blocks, frame_size);
		}

		of_port_state->up = true;
		return ROFL_SUCCESS;
	}

	of_port_state->up = true;
	
	//Prevent race conditions with LINK/STATUS notification threads (bg)
	pthread_rwlock_wrlock(&rwlock);

	ifr.ifr_flags |= IFF_UP;
	if ((rc = ioctl(sd, SIOCSIFFLAGS, &ifr)) < 0){
		XDPD_DEBUG(DRIVER_NAME"[mmap:%s] Unable to bring interface down via ioctl\n",of_port_state->name);
		close(sd);
		pthread_rwlock_unlock(&rwlock);
		return ROFL_FAILURE;
	}
	
	//Release mutex		
	pthread_rwlock_unlock(&rwlock);

	//If tx/rx lines are not created create them
	if(!rx){	
		XDPD_DEBUG_VERBOSE(DRIVER_NAME"[mmap:%s] generating a new mmap_rx for RX\n",of_port_state->name);
		rx = new mmap_rx(std::string(of_port_state->name), 2 * block_size, n_blocks, frame_size);
	}
	if(!tx){
		XDPD_DEBUG_VERBOSE(DRIVER_NAME"[mmap:%s] generating a new mmap_tx for TX\n",of_port_state->name);
		tx = new mmap_tx(std::string(of_port_state->name), block_size, n_blocks, frame_size);
	}


	close(sd);
	return ROFL_SUCCESS;
}

rofl_result_t ioport_mmap::down() {
	
	struct ifreq ifr;
	int sd, rc;

	XDPD_DEBUG_VERBOSE(DRIVER_NAME"[mmap:%s] Trying to bring down\n",of_port_state->name);

	if ((sd = socket(AF_PACKET, SOCK_RAW, 0)) < 0) {
		return ROFL_FAILURE;
	}

	memset(&ifr, 0, sizeof(struct ifreq));
	strcpy(ifr.ifr_name, of_port_state->name);

	if ((rc = ioctl(sd, SIOCGIFINDEX, &ifr)) < 0) {
		return ROFL_FAILURE;
	}

	if ((rc = ioctl(sd, SIOCGIFFLAGS, &ifr)) < 0) {
		close(sd);
		return ROFL_FAILURE;
	}

	//If rx/tx exist, delete them
	if(rx){
		XDPD_DEBUG_VERBOSE(DRIVER_NAME"[mmap:%s] destroying mmap_int for RX\n",of_port_state->name);
		delete rx;
		rx = NULL;
	}
	if(tx){
		XDPD_DEBUG_VERBOSE(DRIVER_NAME"[mmap:%s] destroying mmap_int for TX\n",of_port_state->name);
		delete tx;
		tx = NULL;
	}

	of_port_state->up = false;
	
	if ( !(IFF_UP & ifr.ifr_flags) ) {
		close(sd);
		//Already down.. Silently skip
		return ROFL_SUCCESS;
	}

	//Prevent race conditions with LINK/STATUS notification threads (bg)
	pthread_rwlock_wrlock(&rwlock);

	ifr.ifr_flags &= ~IFF_UP;

	if ((rc = ioctl(sd, SIOCSIFFLAGS, &ifr)) < 0) {
		XDPD_DEBUG(DRIVER_NAME"[mmap:%s] Unable to bring interface down via ioctl\n",of_port_state->name);
		close(sd);
		pthread_rwlock_unlock(&rwlock);
		return ROFL_FAILURE;
	}

	//Release mutex		
	pthread_rwlock_unlock(&rwlock);

	close(sd);

	return ROFL_SUCCESS;
}
