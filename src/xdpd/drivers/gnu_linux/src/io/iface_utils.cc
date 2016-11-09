#include <linux/ethtool.h>
#include <sys/ioctl.h>
#include <ifaddrs.h>
#include <sys/socket.h>
#include <linux/sockios.h>
//#include <netpacket/packet.h>
#include <netinet/in.h>
//#include <net/if.h>
#include <stdio.h>
#include <map>
#include <unistd.h>

//Prototypes
#include <rofl/datapath/pipeline/platform/memory.h>
#include <rofl/datapath/pipeline/openflow/of_switch.h>
#include <rofl/datapath/pipeline/common/datapacket.h>
#include <rofl/datapath/pipeline/physical_switch.h>
#include <rofl/datapath/hal/cmm.h>
#include "iface_utils.h"
#include "iomanager.h"

#include "ports/ioport.h"
#include "ports/mmap/ioport_mmap.h"
#include "ports/vlink/ioport_vlink.h"

#include "../c_logger.h"

using namespace xdpd::gnu_linux;

/*
*
* Port management
*
* All of the functions related to physical port management
*
*/

rofl_result_t update_port_status(char * name){

	struct ifreq ifr;
	int sd, rc;

	switch_port_t *port;
	switch_port_snapshot_t *port_snapshot;

	//Update all ports
	if(update_physical_ports() != ROFL_SUCCESS){
		XDPD_ERR(DRIVER_NAME"[ports] Update physical ports failed \n");
		assert(1);
	}

	port = physical_switch_get_port_by_name(name);

	if(!port)
		return ROFL_SUCCESS; //Port deleted

	ioport* io_port = ((ioport*)port->platform_port_state);

	if ((sd = socket(AF_PACKET, SOCK_RAW, 0)) < 0){
		assert(1);
		return ROFL_FAILURE;
	}

	memset(&ifr, 0, sizeof(struct ifreq));
	strcpy(ifr.ifr_name, port->name);

	if ((rc = ioctl(sd, SIOCGIFINDEX, &ifr)) < 0){
		XDPD_ERR(DRIVER_NAME"[ports] retrieval of interface index of port %s failed\n", name);
		return ROFL_FAILURE;
	}

	//Make sure there are no race conditions between ioctl() calls
	pthread_rwlock_rdlock(&io_port->rwlock);

	//Recover flags
	if ((rc = ioctl(sd, SIOCGIFFLAGS, &ifr)) < 0){
		close(sd);
		pthread_rwlock_unlock(&io_port->rwlock);
		assert(1);
		return ROFL_FAILURE;
	}

	//Release mutex
	pthread_rwlock_unlock(&io_port->rwlock);

	XDPD_DEBUG(DRIVER_NAME"[ports] Interface %s is %s, and link is %s\n", name,( ((IFF_UP & ifr.ifr_flags) > 0) ? "up" : "down"), ( ((IFF_RUNNING & ifr.ifr_flags) > 0) ? "detected" : "not detected"));

	//Update link state
	io_port->set_link_state( ((IFF_RUNNING & ifr.ifr_flags) > 0) );

	if(IFF_UP & ifr.ifr_flags){
		iomanager::bring_port_up(io_port);
	}else{
		iomanager::bring_port_down(io_port);
	}

	//Notify the change of state to the CMM
	port_snapshot = physical_switch_get_port_snapshot(port->name);
	hal_cmm_notify_port_status_changed(port_snapshot);

	return ROFL_SUCCESS;
}


static void fill_port_queues(switch_port_t* port, ioport* io_port){

	unsigned int i;
	char queue_name[PORT_QUEUE_MAX_LEN_NAME];

	//Filling one-by-one the queues that ioport has
	for(i=0;i<io_port->get_num_of_queues();i++){
		snprintf(queue_name, PORT_QUEUE_MAX_LEN_NAME, "%s%d", "queue", i);
		switch_port_add_queue(port, i, (char*)&queue_name, io_port->get_queue_size(i), 0, 0);
	}

}

/*
* Physical port disovery
*/
static rofl_result_t fill_port_admin_and_link_state(switch_port_t* port){

	struct ifreq ifr;
	int sd, rc;

	if ((sd = socket(AF_PACKET, SOCK_RAW, 0)) < 0) {
		return ROFL_FAILURE;
	}

	memset(&ifr, 0, sizeof(struct ifreq));
	strcpy(ifr.ifr_name, port->name);

	if ((rc = ioctl(sd, SIOCGIFINDEX, &ifr)) < 0) {
		return ROFL_FAILURE;
	}

	if ((rc = ioctl(sd, SIOCGIFFLAGS, &ifr)) < 0) {
		close(sd);
		return ROFL_FAILURE;
	}

	//Fill values
	port->up = (IFF_UP & ifr.ifr_flags) > 0;

	if( (IFF_RUNNING & ifr.ifr_flags) > 0){
	}else{
		port->state = PORT_STATE_LINK_DOWN;
	}

	//Close socket
	close(sd);
	return ROFL_SUCCESS;
}

static void fill_port_speeds_capabilities(switch_port_t* port, struct ethtool_cmd* edata){

	bitmap32_t supported_capabilities=0x0, advertised_capabilities=0x0;
	port_features_t current_speed=PORT_FEATURE_10MB_HD;

	// Fill supported features
	if ( edata->supported & SUPPORTED_10baseT_Half )
		supported_capabilities |= PORT_FEATURE_10MB_HD;
	if ( edata->supported & SUPPORTED_10baseT_Full )
		supported_capabilities |= PORT_FEATURE_10MB_FD;
	if ( edata->supported & SUPPORTED_100baseT_Half )
		supported_capabilities |= PORT_FEATURE_100MB_HD;
	if ( edata->supported & SUPPORTED_100baseT_Full )
		supported_capabilities |= PORT_FEATURE_100MB_FD;
	if ( edata->supported & SUPPORTED_1000baseT_Half )
		supported_capabilities |= PORT_FEATURE_1GB_HD;
	if ( edata->supported & SUPPORTED_1000baseT_Full )
		supported_capabilities |= PORT_FEATURE_1GB_FD;
	if ( edata->supported & ADVERTISED_10000baseT_Full )
		supported_capabilities |= PORT_FEATURE_10GB_FD;
	if ( edata->supported & SUPPORTED_Autoneg )
		supported_capabilities |= PORT_FEATURE_AUTONEG;
	if ( edata->supported & SUPPORTED_Pause )
		supported_capabilities |= PORT_FEATURE_PAUSE;
	if ( edata->supported & SUPPORTED_Asym_Pause )
		supported_capabilities |= PORT_FEATURE_PAUSE_ASYM;
	if ( edata->advertising & SUPPORTED_1000baseKX_Full ||
		edata->advertising & SUPPORTED_10000baseKX4_Full ||
		edata->advertising & SUPPORTED_10000baseKR_Full ||
		edata->advertising & SUPPORTED_10000baseR_FEC ||
		edata->advertising & SUPPORTED_20000baseMLD2_Full ||
		edata->advertising & SUPPORTED_20000baseKR2_Full
	)
		advertised_capabilities |= PORT_FEATURE_OTHER;
	if ( edata->supported & SUPPORTED_FIBRE )
		supported_capabilities |= PORT_FEATURE_FIBER;
	//TODO other rates & medium

	// Fill advertised features
	if ( edata->advertising & ADVERTISED_10baseT_Half )
		advertised_capabilities |= PORT_FEATURE_10MB_HD;
	if ( edata->advertising & ADVERTISED_10baseT_Full )
		advertised_capabilities |= PORT_FEATURE_10MB_FD;
	if ( edata->advertising & ADVERTISED_100baseT_Half )
		advertised_capabilities |= PORT_FEATURE_100MB_HD;
	if ( edata->advertising & ADVERTISED_100baseT_Full )
		advertised_capabilities |= PORT_FEATURE_100MB_FD;
	if ( edata->advertising & ADVERTISED_1000baseT_Half )
		advertised_capabilities |= PORT_FEATURE_1GB_HD;
	if ( edata->advertising & ADVERTISED_1000baseT_Full )
		advertised_capabilities |= PORT_FEATURE_1GB_FD;
	if ( edata->advertising & ADVERTISED_10000baseT_Full )
		advertised_capabilities |= PORT_FEATURE_10GB_FD;
	if ( edata->advertising & ADVERTISED_Autoneg )
		advertised_capabilities |= PORT_FEATURE_AUTONEG;
	if ( edata->advertising & ADVERTISED_Pause )
		advertised_capabilities |= PORT_FEATURE_PAUSE;
	if ( edata->advertising & ADVERTISED_Asym_Pause )
		advertised_capabilities |= PORT_FEATURE_PAUSE_ASYM;
	if ( edata->advertising & ADVERTISED_1000baseKX_Full ||
		edata->advertising & ADVERTISED_10000baseKX4_Full ||
		edata->advertising & ADVERTISED_10000baseKR_Full ||
		edata->advertising & ADVERTISED_10000baseR_FEC ||
		edata->advertising & ADVERTISED_20000baseMLD2_Full ||
		edata->advertising & ADVERTISED_20000baseKR2_Full
	)
		advertised_capabilities |= PORT_FEATURE_OTHER;
	if ( edata->advertising & ADVERTISED_FIBRE )
		advertised_capabilities |= PORT_FEATURE_FIBER;
	//TODO other rates & medium


	//Get speed
	uint32_t speed = ethtool_cmd_speed(edata);

	if(speed >= 10 && edata->duplex == DUPLEX_FULL){
		current_speed = PORT_FEATURE_10MB_FD;
	}else if (speed >= 10 && edata->duplex == DUPLEX_HALF){
		current_speed = PORT_FEATURE_10MB_HD;
	}
	if(speed >= 100 && edata->duplex == DUPLEX_FULL){
		current_speed = PORT_FEATURE_100MB_FD;
	}else if (speed >= 100 && edata->duplex == DUPLEX_HALF){
		current_speed = PORT_FEATURE_100MB_HD;
	}
	if(speed >= 1000 && edata->duplex == DUPLEX_FULL){
		current_speed = PORT_FEATURE_1GB_FD;
	}else if (speed >= 1000 && edata->duplex == DUPLEX_HALF){
		current_speed = PORT_FEATURE_1GB_HD;
	}
	if(speed >= 10000 && edata->duplex == DUPLEX_FULL){
		current_speed = PORT_FEATURE_10GB_FD;
	}

	//TODO: properly deduce speeds
	//Filling only with the deduced speed
	switch_port_add_capabilities(&port->curr, advertised_capabilities);
	switch_port_add_capabilities(&port->advertised, advertised_capabilities);
	switch_port_add_capabilities(&port->supported, supported_capabilities);
	switch_port_add_capabilities(&port->peer, advertised_capabilities);

	//Filling speeds
	switch_port_set_current_speed(port, current_speed);
	switch_port_set_current_max_speed(port, current_speed); //TODO: this is not right
}

static switch_port_t* fill_port(int sock, struct ifaddrs* ifa){

	int j;
	struct ethtool_cmd edata;
	struct ifreq ifr;
	struct sockaddr_ll *socll;
	switch_port_t* port;

	//fetch interface info with ethtool
	memset(&ifr, 0, sizeof(struct ifreq));
	strcpy(ifr.ifr_name, ifa->ifa_name);
	memset(&edata,0,sizeof(edata));
	edata.cmd = ETHTOOL_GSET;
	ifr.ifr_data = (char *) &edata;

	//Discard loopback
	if(strncmp("lo",ifa->ifa_name,2) == 0)
		return NULL;

	if (ioctl(sock, SIOCETHTOOL, &ifr)==-1){
		XDPD_WARN(DRIVER_NAME"[ports] WARNING: unable to retrieve MAC address from iface %s via ioctl SIOCETHTOOL. Information will not be filled\n",ifr.ifr_name);
	}

	//Init the port
	port = switch_port_init(ifa->ifa_name, true/*will be overriden afterwards*/, PORT_TYPE_PHYSICAL, PORT_STATE_NONE);
	if(!port)
		return NULL;

	//get the MAC addr.
	socll = (struct sockaddr_ll *)ifa->ifa_addr;
	XDPD_INFO(DRIVER_NAME"[ports] Discovered interface %s mac_addr %02X:%02X:%02X:%02X:%02X:%02X \n",
		ifa->ifa_name,socll->sll_addr[0],socll->sll_addr[1],socll->sll_addr[2],socll->sll_addr[3],
		socll->sll_addr[4],socll->sll_addr[5]);

	for(j=0;j<6;j++)
		port->hwaddr[j] = socll->sll_addr[j];

	//Fill port admin/link state
	if( fill_port_admin_and_link_state(port) == ROFL_FAILURE){
		if(strcmp(port->name,"lo") == 0) //Skip loopback but return success
			return port;

		switch_port_destroy(port);
		return NULL;
	}

	//Fill speeds and capabilities
	fill_port_speeds_capabilities(port, &edata);

	//Initialize MMAP-based port
	//Change this line to use another ioport...
	//ioport* io_port = new ioport_mmapv2(port);
	ioport* io_port = new ioport_mmap(port);

	port->platform_port_state = (platform_port_state_t*)io_port;

	//Fill port queues
	fill_port_queues(port, (ioport*)port->platform_port_state);

	return port;
}

/*
 * Looks in the system physical ports and fills up the switch_port_t sructure with them
 *
 */
rofl_result_t discover_physical_ports(){

	unsigned int i, max_ports;
	switch_port_t* port, **array;
	int sock;

	struct ifaddrs *ifaddr, *ifa;

	/*real way to find interfaces*/
	//getifaddrs(&ifap); -> there are examples on how to get the ip addresses
	if (getifaddrs(&ifaddr) == -1){
		perror("getifaddrs");
		exit(EXIT_FAILURE);
	}

	if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0){
	        perror("socket");
			freeifaddrs(ifaddr);
        	exit(EXIT_FAILURE);
    	}

	for(ifa = ifaddr; ifa != NULL; ifa=ifa->ifa_next){

		if(ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_PACKET)
			continue;

		//Fill port
		port = fill_port(sock, ifa);

		if(!port)
			continue;

		//Adding the
		if( physical_switch_add_port(port) != ROFL_SUCCESS ){
			XDPD_ERR(DRIVER_NAME"[ports] All physical port slots are occupied\n");
			freeifaddrs(ifaddr);
			assert(1);
			return ROFL_FAILURE;
		}

	}

	//This MUST be here and NOT in the previous loop, or the ports will be discovered (duplicated)
	//via update_physical_ports. FIXME: this needs to be implemented properly
	array = physical_switch_get_physical_ports(&max_ports);
	for(i=0; i<max_ports ; i++){
		if(array[i] != NULL){
			//Update status
			if(update_port_status(array[i]->name) != ROFL_SUCCESS){
				XDPD_ERR(DRIVER_NAME"[ports] Unable to retrieve link and/or admin status of the interface\n");
				freeifaddrs(ifaddr);
				assert(1);
				return ROFL_FAILURE;
			}
		}
	}

	freeifaddrs(ifaddr);

	return ROFL_SUCCESS;
}
/*
 * Creates a virtual port pair between two switches
 */
rofl_result_t create_virtual_port_pair(of_switch_t* lsw1, ioport** vport1, of_switch_t* lsw2, ioport** vport2){

	//Names are composed following vlinkX-Y
	//Where X is the virtual link number (0... N-1)
	//Y is the edge 0 (left) 1 (right) of the connectio
	static unsigned int num_of_vlinks=0;
	char port_name[PORT_QUEUE_MAX_LEN_NAME];
	switch_port_t *port1, *port2;
	uint64_t port_capabilities=0x0;
	//uint64_t mac_addr;
	uint16_t randnum = 0;

	//Init the pipeline ports
	snprintf(port_name,PORT_QUEUE_MAX_LEN_NAME, "vlink%u_%u", num_of_vlinks, 0);
	port1 = switch_port_init(port_name, false, PORT_TYPE_VIRTUAL, PORT_STATE_NONE);

	snprintf(port_name,PORT_QUEUE_MAX_LEN_NAME, "vlink%u_%u", num_of_vlinks, 1);
	port2 = switch_port_init(port_name, false, PORT_TYPE_VIRTUAL, PORT_STATE_NONE);

	if(!port1 || !port2){
		free(port1);
		assert(1);
		XDPD_ERR(DRIVER_NAME"[ports] Not enough memory\n");
		return ROFL_FAILURE;
	}

	//Create two ioports
	*vport1 = new ioport_vlink(port1);

	if(!*vport1){
		free(port1);
		free(port2);
		XDPD_ERR(DRIVER_NAME"[ports] Not enough memory\n");
		assert(1);
		return ROFL_FAILURE;
	}

	*vport2 = new ioport_vlink(port2);

	if(!*vport2){
		free(port1);
		free(port2);
		delete *vport1;
		XDPD_ERR(DRIVER_NAME"[ports] Not enough memory\n");
		assert(1);
		return ROFL_FAILURE;
	}

	//Initalize port features(Marking as 1G)
	port_capabilities |= PORT_FEATURE_1GB_FD;

	switch_port_add_capabilities(&port1->curr, (port_features_t)port_capabilities);
	switch_port_add_capabilities(&port1->advertised, (port_features_t)port_capabilities);
	switch_port_add_capabilities(&port1->supported, (port_features_t)port_capabilities);
	switch_port_add_capabilities(&port1->peer, (port_features_t)port_capabilities);
	//mac_addr = 0x0200000000 | (rand() % (sizeof(int)-1));

	randnum = (uint16_t)rand();
	port1->hwaddr[0] = ((uint8_t*)&randnum)[0];
	port1->hwaddr[1] = ((uint8_t*)&randnum)[1];
	randnum = (uint16_t)rand();
	port1->hwaddr[2] = ((uint8_t*)&randnum)[0];
	port1->hwaddr[3] = ((uint8_t*)&randnum)[1];
	randnum = (uint16_t)rand();
	port1->hwaddr[4] = ((uint8_t*)&randnum)[0];
	port1->hwaddr[5] = ((uint8_t*)&randnum)[1];

	// locally administered MAC address
	port1->hwaddr[0] &= ~(1 << 0);
	port1->hwaddr[0] |=  (1 << 1);

	//memcpy(port1->hwaddr, &mac_addr, sizeof(port1->hwaddr));

	switch_port_add_capabilities(&port2->curr, (port_features_t)port_capabilities);
	switch_port_add_capabilities(&port2->advertised, (port_features_t)port_capabilities);
	switch_port_add_capabilities(&port2->supported, (port_features_t)port_capabilities);
	switch_port_add_capabilities(&port2->peer, (port_features_t)port_capabilities);
	//mac_addr = 0x0200000000 | (rand() % (sizeof(int)-1));

	randnum = (uint16_t)rand();
	port2->hwaddr[0] = ((uint8_t*)&randnum)[0];
	port2->hwaddr[1] = ((uint8_t*)&randnum)[1];
	randnum = (uint16_t)rand();
	port2->hwaddr[2] = ((uint8_t*)&randnum)[0];
	port2->hwaddr[3] = ((uint8_t*)&randnum)[1];
	randnum = (uint16_t)rand();
	port2->hwaddr[4] = ((uint8_t*)&randnum)[0];
	port2->hwaddr[5] = ((uint8_t*)&randnum)[1];

	// locally administered MAC address
	port2->hwaddr[0] &= ~(1 << 0);
	port2->hwaddr[0] |=  (1 << 1);

	//memcpy(port2->hwaddr, &mac_addr, sizeof(port1->hwaddr));

	//Add output queues
	fill_port_queues(port1, *vport1);
	fill_port_queues(port2, *vport2);


	//Store platform state on switch ports
	port1->platform_port_state = (platform_port_state_t*)*vport1;
	port2->platform_port_state = (platform_port_state_t*)*vport2;

	//Set cross link
	((ioport_vlink*)*vport1)->set_connected_port((ioport_vlink*)*vport2);
	((ioport_vlink*)*vport2)->set_connected_port((ioport_vlink*)*vport1);

	//Add them to the platform
	if( physical_switch_add_port(port1) != ROFL_SUCCESS ){
		free(port1);
		free(port2);
		delete *vport1;
		delete *vport2;
		XDPD_ERR(DRIVER_NAME"[ports] Unable to add vlink port1 to the physical switch; out of slots?\n");
		assert(1);
		return ROFL_FAILURE;
	}
	if( physical_switch_add_port(port2) != ROFL_SUCCESS ){
		free(port1);
		free(port2);
		delete *vport1;
		delete *vport2;
		XDPD_ERR(DRIVER_NAME"[ports] Unable to add vlink port2 to the physical switch; out of slots?\n");
		assert(1);
		return ROFL_FAILURE;
	}

	//Increment counter and return
	num_of_vlinks++; //TODO: make this atomic jic

	return ROFL_SUCCESS;
}

switch_port_t* get_vlink_pair(switch_port_t* port){
	if(((ioport_vlink*)port->platform_port_state)->connected_port)
		return ((ioport_vlink*)port->platform_port_state)->connected_port->of_port_state;
	else
		return NULL;
}




rofl_result_t destroy_port(switch_port_t* port){

	ioport* ioport_instance;

	if(!port)
		return ROFL_FAILURE;

	//Destroy the inner driver-dependant structures
	ioport_instance = (ioport*)port->platform_port_state;
	delete ioport_instance;

	//Delete port from the pipeline library
	if(physical_switch_remove_port(port->name) == ROFL_FAILURE)
		return ROFL_FAILURE;

	return ROFL_SUCCESS;
}


/*
 * Discovers platform physical ports and fills up the switch_port_t sructures
 *
 */
rofl_result_t destroy_ports(){

	unsigned int max_ports, i;
	switch_port_t** array;

	array = physical_switch_get_physical_ports(&max_ports);
	for(i=0; i<max_ports ; i++){
		if(array[i] != NULL){
			destroy_port(array[i]);
		}
	}

	array = physical_switch_get_virtual_ports(&max_ports);
	for(i=0; i<max_ports ; i++){
		if(array[i] != NULL){
			destroy_port(array[i]);
		}
	}

	//TODO: add tun

	return ROFL_SUCCESS;
}
/*
* Port attachement/detachment
*/


/*
 * Discover new platform ports (usually triggered by bg task manager)
 */
rofl_result_t update_physical_ports(){

	switch_port_t *port, **ports;
	switch_port_snapshot_t *port_snapshot;
	int sock;
	struct ifaddrs *ifaddr, *ifa;
	unsigned int i, max_ports;
	std::map<std::string, struct ifaddrs*> system_ifaces;
	std::map<std::string, switch_port_t*> pipeline_ifaces;

	XDPD_DEBUG_VERBOSE(DRIVER_NAME"[ports] Trying to update the list of physical interfaces...\n");

	/*real way to find interfaces*/
	//getifaddrs(&ifap); -> there are examples on how to get the ip addresses
	if (getifaddrs(&ifaddr) == -1){
		return ROFL_FAILURE;
	}

	if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0){
		return ROFL_FAILURE;
    	}

	//Call the driver to get the list the ports
	ports = physical_switch_get_physical_ports(&max_ports);

	if(!ports){
		close(sock);
		return ROFL_FAILURE;
	}

	//Generate some helpful vectors
	for(i=0;i<max_ports;i++){
		if(ports[i])
			pipeline_ifaces[std::string(ports[i]->name)] = ports[i];

	}
	for(ifa = ifaddr; ifa != NULL; ifa=ifa->ifa_next){
		if(ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_PACKET)
			continue;

		system_ifaces[std::string(ifa->ifa_name)] = ifa;
	}

	//Validate the existance of the ports in the pipeline and remove
	//the ones that are no longer there. If they still exist remove them from ifaces
	for (std::map<std::string, switch_port_t*>::iterator it = pipeline_ifaces.begin(); it != pipeline_ifaces.end(); ++it){
		if (system_ifaces.find(it->first) == system_ifaces.end() ) {
			//Interface has been deleted. Detach and remove
			port = it->second;
			if(!port)
				continue;

			XDPD_INFO(DRIVER_NAME"[ports] Interface %s has been removed from the system. The interface will now be detached from any logical switch it is attached to (if any), and removed from the list of physical interfaces.\n", it->first.c_str());

			//Notify CMM
			port_snapshot = physical_switch_get_port_snapshot(port->name);
			hal_cmm_notify_port_delete(port_snapshot);

			//Detach
			if(port->attached_sw && (hal_driver_detach_port_from_switch(port->attached_sw->dpid, port->name) != HAL_SUCCESS) ){
				XDPD_WARN(DRIVER_NAME"[ports] WARNING: unable to detach port %s from switch. This can lead to an unknown behaviour\n", it->first.c_str());
				assert(1);
			}
			//Destroy and remove from the list of physical ports
			destroy_port(port);
		}
		system_ifaces.erase(it->first);
	}

	//Add remaining "new" interfaces (remaining interfaces in system_ifaces map
	for (std::map<std::string, struct ifaddrs*>::iterator it = system_ifaces.begin(); it != system_ifaces.end(); ++it){
		//Fill port
		port = fill_port(sock,  it->second);
		if(!port){
			//XDPD_ERR(DRIVER_NAME"[ports] Unable to initialize newly discovered interface %s\n", it->first.c_str());
			continue;
		}

		//Adding the
		if( physical_switch_add_port(port) != ROFL_SUCCESS ){
			XDPD_ERR(DRIVER_NAME"[ports] Unable to add port %s to physical switch. Not enough slots?\n", it->first.c_str());
			freeifaddrs(ifaddr);
			continue;
		}

		//Notify CMM
		port_snapshot = physical_switch_get_port_snapshot(port->name);
		hal_cmm_notify_port_add(port_snapshot);

	}

	XDPD_DEBUG_VERBOSE(DRIVER_NAME"[ports] Update of interfaces done.\n");

	freeifaddrs(ifaddr);
	close(sock);

	return ROFL_SUCCESS;
}
