/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef SWITCH_MANAGER_H
#define SWITCH_MANAGER_H 

#include <map>
#include <list>
#include <string>
#include <iostream>

#include <pthread.h>
#include <stdio.h>
#include <limits.h>
#include <endian.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdexcept>

#include <rofl_datapath.h>
#include <rofl/common/caddress.h>

#include <rofl/datapath/pipeline/common/datapacket.h>
#include <rofl/datapath/pipeline/openflow/of_switch.h>
#include <rofl/datapath/pipeline/openflow/openflow1x/pipeline/of1x_flow_entry.h>

#include "xdpd/common/exception.h"
#include "xdpd/common/csocket.h"
#include "xdpd/common/cparams.h"
#include "xdpd/common/logging.h"

//Snapshot
#include "snapshots/switch_snapshot.h"
#include "snapshots/flow_entry_snapshot.h"
#include "snapshots/group_mod_snapshot.h"

/**
* @file switch_manager.h
* @author Marc Sune<marc.sune (at) bisdn.de>
* @author Tobias Jungel<tobias.jungel (at) bisdn.de>
*
* @brief Logical Switch Instance (LSI) management API file.
*/

namespace xdpd {

class eOfSmBase				: public xdpd::exception {
public:
	eOfSmBase(
			const std::string& __arg = std::string("")) :
				xdpd::exception(__arg)
	{};
};	// base error class for all switch_manager related errors
class eOfSmGeneralError			: public eOfSmBase {};
class eOfSmErrorOnCreation		: public eOfSmBase {};
class eOfSmExists			: public eOfSmBase {};
class eOfSmDoesNotExist			: public eOfSmBase {};
class eOfSmNotFound			: public eOfSmBase {};
class eOfSmFlowModBadCommand		: public eOfSmBase {};
class eOfSmPipelineBadTableId		: public eOfSmBase {};
class eOfSmPipelineTableFull		: public eOfSmBase {};
class eOfSmPortModBadPort		: public eOfSmBase {};
class eOfSmVersionNotSupported		: public eOfSmBase {};
class eOfSmUnknownSocketType		: public eOfSmBase {};
class eOfSmExperimentalNotSupported	: public eOfSmBase {};

//Fwd declaration
class openflow_switch;

/**
* @brief Logical Switch (LS) management API.
* 
* The switch manager API is a C++ interface that can be consumed
* by the add-on management modules for general logical switch management
* (e.g. create/destroy logical switches)
* @ingroup cmm_mgmt
*/
class switch_manager {
public:

	//
	// Switch management
	//

	/**
	 * @brief	static factory method for creating a logical switch (LS)
	 *
	 * This method creates a new Openflow Logical Switch instance with dpid and dpname.
	 *
	 * @param dpid data path element id
	 * @param dpname name of this data path element (local significance only)
	 */
	static openflow_switch* create_switch(of_version_t version,
					uint64_t dpid,
					std::string const& dpname,
					unsigned int num_of_tables,
					int* ma_list,
					int reconnect_start_timeout,
					enum xdpd::csocket::socket_type_t socket_type,
					const xdpd::cparams& params);


	/**
	 * @brief	static method that deletes 
	 *
	 * this method destroy the logical switch referenced by dpid. it also
	 * closes all the active connections and end-point listening sockets
	 *
	 * @param dpid data path element id
	 */
	static void destroy_switch(uint64_t dpid);

	/**
	 * @brief	static method that deletes all switches 
	 *
	 */
	static void destroy_all_switches(void);



	//Add missing attach port, detach and bring up down port


	/**
	 * Lists datapath names
	 */
	static std::list<std::string> list_sw_names(void);

	/**
	 * Return true if switch exists
	 */
	static bool exists(uint64_t dpid);

	/**
	 * Return true if switch exists with the name 'name'
	 */
	static bool exists_by_name(std::string& name);
		
	
	/**
	 * Return the dpid of the switch 
	 */
	static uint64_t get_switch_dpid(std::string const& name);

	/**
	* Get switch information (snapshot)
	* @param snapshot Snapshot of the switch to be filled in. 
	*/
	static void get_switch_info(uint64_t dpid, openflow_switch_snapshot& snapshot);

	/**
	* Get list of switch table flow entries currently installed 
	* @param flows List of flows installed. 
	*/
	static void get_switch_table_flows(uint64_t dpid, uint8_t table_id /*TODO: Add filtering */, std::list<flow_entry_snapshot>& flows);
	
	/**
	* Get list of switch group table entries currently installed 
	* @param flows List of group mods installed. 
	*/
	static void get_switch_group_mods(uint64_t dpid, std::list<openflow_group_mod_snapshot>& group_mods);
	
	/**
	 * List available matching algorithms
	 */
	static std::list<std::string> list_matching_algorithms(of_version_t of_version);

	//
	// Switch controller setup
	//

	/**
	 * connect to controller
	 */
	static void rpc_connect_to_ctl(uint64_t dpid, enum xdpd::csocket::socket_type_t socket_type, xdpd::cparams const& socket_params);

	/**
	 * disconnect from from controller
	 */
	static void rpc_disconnect_from_ctl(uint64_t dpid, enum xdpd::csocket::socket_type_t socket_type, xdpd::cparams const& socket_params);

	//
	// Other configuration parameters
	//

	/**
	* Change Packet In Rate Limiter(PIRL) rate, in packet_in events per second
	*
	* @param max_rate Maximum rate in pkt_in/s. It should be higher or equal than the minimum PIRL rate. Use pirl::PIRL_DISABLED to disable PIRL subsytem.
	*/
	static void reconfigure_pirl(uint64_t dpid, const int max_rate);


	//
	//CMM demux
	//
	
	static rofl_result_t __notify_port_attached(const switch_port_snapshot_t* port_snapshot);	
	static rofl_result_t __notify_port_status_changed(const switch_port_snapshot_t* port_snapshot);	
	static rofl_result_t __notify_port_detached(const switch_port_snapshot_t* port_snapshot);	
	static rofl_result_t __process_of1x_packet_in(uint64_t dpid,
					uint8_t table_id,
					uint8_t reason,
					uint32_t in_port,
					uint32_t buffer_id,
					uint64_t cookie,
					uint8_t* pkt_buffer,
					uint32_t buf_len,
					uint16_t total_len,
					packet_matches_t* matches);
	static rofl_result_t __process_of1x_flow_removed(uint64_t dpid, 
					uint8_t reason, 	
					of1x_flow_entry_t* removed_flow_entry);

private:
	
	/* Static members */
	//Switch container
	static std::map<uint64_t, openflow_switch*> switchs; 
	static uint64_t dpid_under_destruction;

	//Shall never be used except for the cmm
	static openflow_switch* __get_switch_by_dpid(uint64_t dpid);	

	//Default addresses
	static const rofl::caddress_in4 controller_addr;
	static const uint16_t controller_port;
	static const rofl::caddress_in4 binding_addr;
	static const uint16_t binding_port;

	static pthread_mutex_t mutex;
	static pthread_rwlock_t rwlock;

};




}// namespace xdpd

#endif /* SWITCH_MANAGER_H_ */
