/*
 * openflow10_switch.cc
 *
 *  Created on: 06.09.2013
 *      Author: andreas
 */

#include "openflow10_switch.h"

#include <rofl/datapath/hal/openflow/openflow1x/of1x_driver.h>
#include <rofl/datapath/hal/openflow/openflow1x/of1x_cmm.h>

using namespace xdpd;

/*
* Constructor and destructor for the openflow 1.0 switch
*/
openflow10_switch::openflow10_switch(uint64_t dpid,
				std::string const& dpname,
				unsigned int num_of_tables,
				int* ma_list,
				int reconnect_start_timeout,
				const rofl::openflow::cofhello_elem_versionbitmap& versionbitmap,
				enum xdpd::csocket::socket_type_t socket_type,
				xdpd::cparams const& socket_params) throw (eOfSmVersionNotSupported)
		: openflow_switch(dpid, dpname, OF_VERSION_10, num_of_tables)
{

	if (hal_driver_create_switch((char*)dpname.c_str(),
					     dpid, OF_VERSION_10, num_of_tables, ma_list) != HAL_SUCCESS){
		//WRITELOG(CDATAPATH, ERROR, "of10_endpoint::of10_endpoint() "
		//		"failed to allocate switch instance in HAL, aborting");

		throw eOfSmErrorOnCreation();
	}

	//Initialize the endpoint, and launch control channel
	endpoint = new of10_endpoint(this, reconnect_start_timeout, versionbitmap, socket_type, socket_params);
}


openflow10_switch::~openflow10_switch(){

	//Now safely destroy the endpoint
	delete endpoint;

	//Destroy forwarding plane state
	hal_driver_destroy_switch_by_dpid(dpid);
}

/* Public interfaces for receving async messages from the driver */
rofl_result_t openflow10_switch::process_packet_in(uint8_t table_id,
					uint8_t reason,
					uint32_t in_port,
					uint32_t buffer_id,
					uint64_t cookie,
					uint8_t* pkt_buffer,
					uint32_t buf_len,
					uint16_t total_len,
					packet_matches_t* matches){

	return ((of10_endpoint*)endpoint)->process_packet_in(table_id,
					reason,
					in_port,
					buffer_id,
					cookie,
					pkt_buffer,
					buf_len,
					total_len,
					matches);
}

rofl_result_t openflow10_switch::process_flow_removed(uint8_t reason, of1x_flow_entry_t* removed_flow_entry){
	return ((of10_endpoint*)endpoint)->process_flow_removed(reason, removed_flow_entry);
}
