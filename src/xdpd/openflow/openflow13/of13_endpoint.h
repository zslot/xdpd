/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
* @file of13_endpoint.h
* @author Andreas Koepsel<andreas.koepsel (at) bisdn.de>
* @author Marc Sune<marc.sune (at) bisdn.de>
* @author Victor Alvarez<victor.alvarez (at) bisdn.de>
* @author Tobias Jungel<tobias.jungel (at) bisdn.de>
*
* @brief OF1.3 endpoint implementation
*/

#ifndef OF13_ENDPOINT_H
#define OF13_ENDPOINT_H 

#include <rofl/datapath/pipeline/openflow/openflow1x/of1x_switch.h>
#include "../openflow_switch.h"
#include "../of_endpoint.h"
#include "../../management/switch_manager.h"

using namespace rofl;

namespace xdpd {

/**
* @brief of13_endpoint is an OpenFlow 1.3 OF agent implementation
* @ingroup cmm_of
**/
class of13_endpoint : public of_endpoint {
	

public:

	//Main constructor
	of13_endpoint(
			openflow_switch* sw,
			int reconnect_start_timeout,
			const rofl::openflow::cofhello_elem_versionbitmap& versionbitmap,
			enum xdpd::csocket::socket_type_t socket_type,
			xdpd::cparams const& socket_params) throw (eOfSmErrorOnCreation);

	/**
	 *
	 */
	rofl_result_t
	process_packet_in(
			uint8_t table_id,
			uint8_t reason,
			uint32_t in_port,
			uint32_t buffer_id,
			uint64_t cookie,
			uint8_t* pkt_buffer,
			uint32_t buf_len,
			uint16_t total_len,
			packet_matches_t* matches);


	/**
	 *
	 */
	rofl_result_t
	process_flow_removed(
			uint8_t reason,
			of1x_flow_entry *removed_flow_entry);

	/*
	* Port notifications
	*/

	virtual	rofl_result_t notify_port_attached(const switch_port_t* port);
	
	virtual rofl_result_t notify_port_detached(const switch_port_t* port);

	virtual rofl_result_t notify_port_status_changed(const switch_port_t* port);

private:

	/* *
	 ** This section is in charge of the handling of the OF messages
	 ** comming from the cofctl(OF endpoints). These are version specific
	 ** and must be implemented by the derived class (ofXX_dphcl) 
	 **/

	/** Handle OF features request. To be overwritten by derived class.
	 *
	 * OF FEATURES.requests are handled by the crofbase base class in method
	 * crofbase::send_features_reply(). However,
	 * this method handle_features_request() may be overloaded by a derived class to get a notification
	 * upon reception of a FEATURES.request from the controlling entity.
	 * Default behaviour is to remove the packet from the heap.
	 * The OF packet must be removed from heap by the overwritten method.
	 *
	 * @param pack OF packet received from controlling entity.
	 */
	virtual void
	handle_features_request(rofl::crofctl& ctl, const rofl::cauxid& auxid, rofl::openflow::cofmsg_features_request& msg);

	/** Handle OF get-config request. To be overwritten by derived class.
	 *
	 * Called from within crofbase::fe_down_get_config_request().
	 * The OF packet must be removed from heap by the overwritten method.
	 *
	 * @param ctrl cofdpath instance from whom the GET-CONFIG.request was received.
	 * @pack OF GET-CONFIG.request packet received from controller
	 */
	virtual void
	handle_get_config_request(rofl::crofctl& ctl, const rofl::cauxid& auxid, rofl::openflow::cofmsg_get_config_request& msg);

	/**
	 *
	 */
	virtual void
	handle_desc_stats_request(rofl::crofctl& ctl, const rofl::cauxid& auxid, rofl::openflow::cofmsg_desc_stats_request& msg);


	/**
	 *
	 */
	virtual void
	handle_table_stats_request(rofl::crofctl& ctl, const rofl::cauxid& auxid, rofl::openflow::cofmsg_table_stats_request& msg);


	/**
	 *
	 */
	virtual void
	handle_port_stats_request(rofl::crofctl& ctl, const rofl::cauxid& auxid, rofl::openflow::cofmsg_port_stats_request& msg);


	/**
	 *
	 */
	virtual void
	handle_flow_stats_request(rofl::crofctl& ctl, const rofl::cauxid& auxid, rofl::openflow::cofmsg_flow_stats_request& msg);


	/**
	 *
	 */
	virtual void
	handle_aggregate_stats_request(rofl::crofctl& ctl, const rofl::cauxid& auxid, rofl::openflow::cofmsg_aggr_stats_request& msg);


	/**
	 *
	 */
	virtual void
	handle_queue_stats_request(rofl::crofctl& ctl, const rofl::cauxid& auxid, rofl::openflow::cofmsg_queue_stats_request& msg);


	/**
	 *
	 */
	virtual void
	handle_group_stats_request(rofl::crofctl& ctl, const rofl::cauxid& auxid, rofl::openflow::cofmsg_group_stats_request& msg);


	/**
	 *
	 */
	virtual void
	handle_group_desc_stats_request(rofl::crofctl& ctl, const rofl::cauxid& auxid, rofl::openflow::cofmsg_group_desc_stats_request& msg);


	/**
	 *
	 */
	virtual void
	handle_group_features_stats_request(rofl::crofctl& ctl, const rofl::cauxid& auxid, rofl::openflow::cofmsg_group_features_stats_request& msg);


	/**
	 *
	 */
	virtual void
	handle_meter_stats_request(rofl::crofctl& ctl, const rofl::cauxid& auxid, rofl::openflow::cofmsg_meter_stats_request& msg);


	/**
	 *
	 */
	virtual void
	handle_meter_config_stats_request(rofl::crofctl& ctl, const rofl::cauxid& auxid, rofl::openflow::cofmsg_meter_config_stats_request& msg);


	/**
	 *
	 */
	virtual void
	handle_meter_features_stats_request(rofl::crofctl& ctl, const rofl::cauxid& auxid, rofl::openflow::cofmsg_meter_features_stats_request& msg);


	/**
	 *
	 */
	virtual void
	handle_table_features_stats_request(rofl::crofctl& ctl, const rofl::cauxid& auxid, rofl::openflow::cofmsg_table_features_stats_request& msg);


	/**
	 *
	 */
	virtual void
	handle_port_desc_stats_request(rofl::crofctl& ctl, const rofl::cauxid& auxid, rofl::openflow::cofmsg_port_desc_stats_request& msg);


	/**
	 *
	 */
	virtual void
	handle_experimenter_stats_request(rofl::crofctl& ctl, const rofl::cauxid& auxid, rofl::openflow::cofmsg_experimenter_stats_request& msg);

	/** Handle OF packet-out messages. To be overwritten by derived class.
	 *
	 * Called upon reception of a PACKET-OUT.message from the controlling entity.
	 * The OF packet must be removed from heap by the overwritten method.
	 *
	 * @param pack PACKET-OUT.message packet received from controller.
	 */
	virtual void
	handle_packet_out(rofl::crofctl& ctl, const rofl::cauxid& auxid, rofl::openflow::cofmsg_packet_out& msg);

	/** Handle OF barrier request. To be overwritten by derived class.
	 *
	 * Called upon reception of a BARRIER.request from the controlling entity.
	 * The OF packet must be removed from heap by the overwritten method.
	 *
	 * @param pack BARRIER.request packet received from controller.
	 */
	virtual void
	handle_barrier_request(rofl::crofctl& ctl, const rofl::cauxid& auxid, rofl::openflow::cofmsg_barrier_request& msg);

	/** Handle OF flow-mod message. To be overwritten by derived class.
	 *
	 * Called upon reception of a FLOW-MOD.message from the controlling entity.
	 * The OF packet must be removed from heap by the overwritten method.
	 *
	 * @param pack FLOW-MOD.message packet received from controller.
	 */
	virtual void
	handle_flow_mod(rofl::crofctl& ctl, const rofl::cauxid& auxid, rofl::openflow::cofmsg_flow_mod& msg);

	/** Handle OF group-mod message. To be overwritten by derived class.
	 *
	 * Called upon reception of a GROUP-MOD.message from the controlling entity.
	 * The OF packet must be removed from heap by the overwritten method.
	 *
	 * @param pack GROUP-MOD.message packet received from controller.
	 */
	virtual void
	handle_group_mod(rofl::crofctl& ctl, const rofl::cauxid& auxid, rofl::openflow::cofmsg_group_mod& msg);

	/** Handle OF table-mod message. To be overwritten by derived class.
	 *
	 * Called upon reception of a TABLE-MOD.message from the controlling entity.
	 * The OF packet must be removed from heap by the overwritten method.
	 *
	 * @param pack TABLE-MOD.message packet received from controller.
	 */
	virtual void
	handle_table_mod(rofl::crofctl& ctl, const rofl::cauxid& auxid, rofl::openflow::cofmsg_table_mod& msg);

	/** Handle OF port-mod message. To be overwritten by derived class.
	 *
	 * Called upon reception of a PORT-MOD.message from the controlling entity.
	 * The OF packet must be removed from heap by the overwritten method.
	 *
	 * @param pack PORT-MOD.message packet received from controller.
	 */
	virtual void
	handle_port_mod(rofl::crofctl& ctl, const rofl::cauxid& auxid, rofl::openflow::cofmsg_port_mod& msg);

	/** Handle OF set-config message. To be overwritten by derived class.
	 *
	 * Called upon reception of a SET-CONFIG.message from the controlling entity.
	 * The OF packet must be removed from heap by the overwritten method.
	 *
	 * @param pack SET-CONFIG.message packet received from controller.
	 */
	virtual void
	handle_set_config(rofl::crofctl& ctl, const rofl::cauxid& auxid, rofl::openflow::cofmsg_set_config& msg);

	/** Handle OF queue-get-config request. To be overwritten by derived class.
	 *
	 * Called upon reception of a QUEUE-GET-CONFIG.reply from a datapath entity.
	 * The OF packet must be removed from heap by the overwritten method.
	 *
	 * @param sw cofswitch instance from whom a QUEUE-GET-CONFIG.reply was received
	 * @param pack QUEUE-GET-CONFIG.reply packet received from datapath
	 */
	virtual void
	handle_queue_get_config_request(rofl::crofctl& ctl, const rofl::cauxid& auxid, rofl::openflow::cofmsg_queue_get_config_request& msg);

	/** Handle OF experimenter message. To be overwritten by derived class.
	 *
	 * Called upon reception of a VENDOR.message from the controlling entity.
	 * The OF packet must be removed from heap by the overwritten method.
	 *
	 * @param pack VENDOR.message packet received from controller.
	 */
	virtual void
	handle_experimenter_message(rofl::crofctl& ctl, const rofl::cauxid& auxid, rofl::openflow::cofmsg_experimenter& msg);

	/** Handle OF meter-mod message. To be overwritten by derived class.
	 *
	 * Called upon reception of a meter-mod.message from the controlling entity.
	 *
	 * @param pack VENDOR.message packet received from controller.
	 */
	virtual void
	handle_meter_mod(rofl::crofctl& ctl, const cauxid& auxid, rofl::openflow::cofmsg_meter_mod& msg);

	/** Handle new ctrl
	 *
	 * Called upon creation of a new cofctrl instance.
	 *
	 * @param ctrl new cofctrl instance
	 */
	virtual void
	handle_ctl_open(rofl::crofctl& ctrl);

	/** Handle close event on ctrl
	 *
	 * Called upon deletion of a cofctrl instance
	 *
	 * @param ctrl cofctrl instance to be deleted
	 */
	virtual void
	handle_ctl_close(const rofl::cctlid& id);

	/**
	 * @brief 	Called when a control connection (main or auxiliary) has been established.
	 *
	 * @param ctl controller instance
	 * @param auxid connection identifier (main: 0)
	 */
	virtual void
	handle_conn_established(
			rofl::crofctl& ctl,
			const rofl::cauxid& auxid);

	/**
	 * @brief 	Called when a control connection (main or auxiliary) has been terminated by the peer entity.
	 *
	 * @param ctl controller instance
	 * @param auxid connection identifier (main: 0)
	 */
	virtual void
	handle_conn_terminated(
			rofl::crofctl& ctl,
			const rofl::cauxid& auxid);

	/**
	 * @brief 	Called when an attempt to establish a control connection has been refused.
	 *
	 * This event occurs when the C-library's connect() system call fails
	 * with the ECONNREFUSED error code. This indicates typically a problem on
	 * the remote site.
	 *
	 * @param ctl controller instance
	 * @param auxid connection identifier (main: 0)
	 */
	virtual void
	handle_conn_refused(
			rofl::crofctl& ctl,
			const rofl::cauxid& auxid);

	/**
	 * @brief 	Called when an attempt to establish a control connection has been failed.
	 *
	 * This event occurs when some failure occures while calling the underlying
	 * C-library connect() system call, e.g., no route to destination, etc. This may
	 * indicate a local configuration problem inside or outside of the application.
	 *
	 * @param ctl controller instance
	 * @param auxid connection identifier (main: 0)
	 */
	virtual void
	handle_conn_failed(
			rofl::crofctl& ctl,
			const rofl::cauxid& auxid);

	/**
	 * @brief	Called when a negotiation failed with a peer controller entity
	 *
	 * @param ctl controller instance
	 * @param auxid control connection identifier (main: 0)
	 */
	virtual void
	handle_conn_negotiation_failed(
			rofl::crofctl& ctl,
			const rofl::cauxid& auxid);

	/**
	 * @brief	Called when a congestion situation on the control connection occurs
	 *
	 * @param ctl controller instance
	 * @param auxid control connection identifier (main: 0)
	 */
	virtual void
	handle_conn_congestion_occured(
			rofl::crofctl& ctl,
			const rofl::cauxid& auxid);

	/**
	 * @brief	Called when a congestion situation on the control connection has been solved
	 *
	 * @param ctl controller instance
	 * @param auxid control connection identifier (main: 0)
	 */
	virtual void
	handle_conn_congestion_solved(
			rofl::crofctl& ctl,
			const rofl::cauxid& auxid);

	/**
	 * @name 	flow_mod_add
	 * @brief 	Add a flow mod received from controller to flow-table
	 *
	 * This method adds a new flow-table-entry to the flow-table of
	 * thw logical switch.
	 *
	 * @param ctl Pointer to cofctl instance representing the controller from whom we received the flow-mod
	 * @param pack Pointer to cofpacket instance storing the flow-mod command received. This must not be freed at the end
	 * of this method!
	 * @return void
	 */
	void
	flow_mod_add(
			crofctl& ctl,
			rofl::openflow::cofmsg_flow_mod& pack);



	/**
	 * @name 	flow_mod_modify
	 * @brief 	Modify a flow mod received from controller to flow-table
	 *
	 * This method adds a new flow-table-entry to the flow-table of
	 * thw logical switch.
	 *
	 * @param ctl Pointer to cofctl instance representing the controller from whom we received the flow-mod
	 * @param pack Pointer to cofpacket instance storing the flow-mod command received. This must not be freed at the end
	 * of this method!
	 * @return void
	 */
	void
	flow_mod_modify(
			crofctl& ctl,
			rofl::openflow::cofmsg_flow_mod& pack,
			bool strict);


	/**
	 * @name 	flow_mod_delete
	 * @brief 	Add a flow mod received from controller to flow-table
	 *
	 * This method adds a new flow-table-entry to the flow-table of
	 * thw logical switch.
	 *
	 * @param ctl Pointer to cofctl instance representing the controller from whom we received the flow-mod
	 * @param pack Pointer to cofpacket instance storing the flow-mod command received. This must not be freed at the end
	 * of this method!
	 * @return void
	 */
	void
	flow_mod_delete(
			crofctl& ctl,
			rofl::openflow::cofmsg_flow_mod& pack,
			bool strict);


	/**
	 * @name	port_set_config
	 * @brief
	 *
	 * This method changes a port configuration.
	 *
	 * @param dpid switch dpid
	 * @param portno OF port number
	 * @param config parameter received from message OFPT_PORT_MOD
	 * @param mask parameter received from message OFPT_PORT_MOD
	 * @param advertise parameter received from message OFPT_PORT_MOD
	 */
	void
	port_set_config(
			uint64_t dpid,
			uint32_t portno,
			uint32_t config,
			uint32_t mask,
			uint32_t advertise);

};

}// namespace rofl

#endif /* OF13_ENDPOINT_H_ */
