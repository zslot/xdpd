#include "time_measurements.h"
#include "../io/datapacketx86.h"

//Initialize global stats
time_measurements_t global_measurements = {0};

const char* stage_names[TM_MAX] = {"Allocation\t",		//S0
			"RX memcpy\t\t", 			//S1
			"Header classification\t", 		//S2
			"Pre process to switch",		//S3	
			"Pipeline matches init\t",		//S4
			"Pipeline processing (match)",		//SA5_PRE
			"Enqueue output port (SUCCESS)",	//SA5_SUCCESS
			"Enqueue output port (FAILURE)",	//SA5_FAILURE
			"Pipeline processing (no-match)",	//SB5_PRE
			"Enqueue PKT_IN queue (SUCCESS)",	//SB5_SUCCESS
			"Enqueue PKT_IN queue (FAILURE)",	//SB5_FAILURE
			"Stayed in output queue(**)",		//SA6
			"TX memcpy\t\t",			//SA7
			};

static void tm_dump_row(const char* message, uint64_t accumulated_stage_ticks, double path_ticks, uint64_t stage_number_of_packets){

	double stage_ticks=0.0;
	if(stage_number_of_packets)
		stage_ticks = (double)(accumulated_stage_ticks/stage_number_of_packets);
	
	XDPD_INFO("%s\t%10.1f\t\t%05.02f\t\t%u\n", message, stage_ticks, (stage_ticks/path_ticks)*100, stage_number_of_packets);
}

void tm_dump_measurements(void){

	unsigned int i;	
	double ticks_fast_path=0, ticks_slow_path=0;

	XDPD_INFO("\nGNU/Linux time measurements summary\n");
	XDPD_INFO("-----------------------------------\n\n");

	XDPD_INFO("Stage name \t\t\t| Average ticks | %% overall ticks(*) | # pkts\n");
	XDPD_INFO("*****************************************************************************\n");

	//Calculate average ticks
	for(i=0;i<TM_MAX;i++){
	
		switch(i){
			case TM_S0:
				break;
			case TM_S1:
			case TM_S2:
			case TM_S3:
			case TM_S4:
				//Common part
				if(global_measurements.total_pkts > 0.0){
					ticks_fast_path += (double)(global_measurements.accumulated[i]/global_measurements.total_pkts);
					ticks_slow_path += (double)(global_measurements.accumulated[i]/global_measurements.total_pkts);
				}
				break;
			case TM_SA5_FAILURE:
			case TM_SB5_FAILURE:
				break;
			case TM_SB5_PRE:
			case TM_SB5_SUCCESS:
				if(global_measurements.total_pktin_pkts > 0.0)
					ticks_slow_path += (double)(global_measurements.accumulated[i]/global_measurements.total_pktin_pkts);
				break;
			case TM_SA5_PRE:
			case TM_SA5_SUCCESS:
			case TM_SA6:
			case TM_SA7:
				if(global_measurements.total_output_pkts > 0.0)
					ticks_fast_path += (double)(global_measurements.accumulated[i]/global_measurements.total_output_pkts);
				break;
		}
	}

	//Do the calculations for stage time
	for(i=0;i<TM_MAX;i++){
			
		switch(i){
			case TM_S0:
				break;
			case TM_S1:
				tm_dump_row(stage_names[i], global_measurements.accumulated[i],  ticks_fast_path, global_measurements.total_pkts);
				break;
			case TM_S2:
				tm_dump_row(stage_names[i], global_measurements.accumulated[i], ticks_fast_path, global_measurements.total_pkts);
				break;
			case TM_S3:
				break;
			case TM_S4:
				tm_dump_row(stage_names[i], global_measurements.accumulated[i], ticks_fast_path, global_measurements.total_pkts);
				break;
			case TM_SA5_PRE:
				tm_dump_row(stage_names[i], global_measurements.accumulated[i], ticks_fast_path, global_measurements.total_pkts);
				break;
			case TM_SA5_SUCCESS:
				tm_dump_row(stage_names[i], global_measurements.accumulated[i], ticks_fast_path, global_measurements.total_pkts - global_measurements.total_pktin_pkts - global_measurements.total_SA5_dropped_pkts);
				break;
			case TM_SA5_FAILURE:
				tm_dump_row(stage_names[i], global_measurements.accumulated[i], ticks_fast_path, global_measurements.total_SA5_dropped_pkts);
				break;
			case TM_SB5_PRE:
				tm_dump_row(stage_names[i], global_measurements.accumulated[i], ticks_slow_path,global_measurements.total_pktin_pkts);
				break;
			case TM_SB5_SUCCESS:
				tm_dump_row(stage_names[i], global_measurements.accumulated[i], ticks_slow_path,global_measurements.total_pktin_pkts);
				break;
			case TM_SB5_FAILURE:
				tm_dump_row(stage_names[i], global_measurements.accumulated[i], ticks_slow_path,global_measurements.total_SB5_dropped_pkts);
				break;
			case TM_SA6:
				tm_dump_row(stage_names[i], global_measurements.accumulated[i], ticks_fast_path, global_measurements.total_output_pkts);
				break;
			case TM_SA7:
				tm_dump_row(stage_names[i], global_measurements.accumulated[i], ticks_fast_path, global_measurements.total_output_pkts);
				break;
		}
	}	
	
	XDPD_INFO("\n");
	XDPD_INFO("* Always refered to the average fast path transit ticks, except for slow path sections.\n");
	XDPD_INFO("** Includes enqueue ticks.\n");
	XDPD_INFO("\n\n");
	
	//Final stats
	XDPD_INFO("Average transit ticks in the fast path: %.1f\n", ticks_fast_path);
	XDPD_INFO("Average transit ticks in the slow path(until PKT_IN enqueue): %.1f\n", ticks_slow_path);
	XDPD_INFO("Total RX packets: %u (100%)\n",global_measurements.total_pkts);
	XDPD_INFO("Total TX packets (excluding PKT_OUTs): %u (%.2f%%)\n",global_measurements.total_output_pkts, ((float)global_measurements.total_output_pkts/global_measurements.total_pkts*100));
	XDPD_INFO("Total PKT_IN packets: %u (%.2f%%)\n",global_measurements.total_pktin_pkts, ((float)global_measurements.total_pktin_pkts/global_measurements.total_pkts*100) );
	XDPD_INFO("Total Dropped trying to enqueue to output port(s): %u (%.2f%%)\n",global_measurements.total_SA5_dropped_pkts, ((float)global_measurements.total_SA5_dropped_pkts/global_measurements.total_pkts*100));
	XDPD_INFO("Total Dropped trying to enqueue to PKT_IN queue: %u (%.2f%%)\n\n",global_measurements.total_SB5_dropped_pkts, ((float)global_measurements.total_SB5_dropped_pkts/global_measurements.total_pkts*100));
}

#ifdef ENABLE_TIME_MEASUREMENTS
//Dumps the path of a packet through the GNU/Linux driver (useful when debugging)
void tm_dump_pkt(datapacket_t* pkt){

	unsigned int i,j;	
	time_measurements_t* tm = &((xdpd::gnu_linux::datapacketx86*)pkt->platform_state)->tm_state;
	
	XDPD_INFO("Dumping path of pkt (%p)", pkt);
	if(tm->pkt_out)
		XDPD_INFO(" [PKT_OUT]");
	XDPD_INFO(":\n");

	//Do the calculations for stage time
	for(i=0,j=0;i<TM_MAX;i++){
			
		if(!tm->current[i])
			continue;

		switch(i){
			case TM_S0:
			case TM_S1:
			case TM_S2:
			case TM_S3:
			case TM_S4:
			case TM_SA5_PRE:
			case TM_SA5_SUCCESS:
			case TM_SA5_FAILURE:
			case TM_SB5_PRE:
			case TM_SB5_SUCCESS:
			case TM_SB5_FAILURE:
			case TM_SA6:
			case TM_SA7:
				XDPD_INFO("[%u] %s(%u)\n", j, stage_names[i], tm->current[i]);
				j++;
				break;
		}
	}
		
}
#endif
