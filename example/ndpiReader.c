/*
 * ndpiReader.c
 *
 * Copyright (C) 2011-18 - ntop.org
 *
 * nDPI is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * nDPI is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with nDPI.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifdef HAVE_CONFIG_H
#include "ndpi_config.h"
#endif

#ifdef linux
#define _GNU_SOURCE
#include <sched.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#ifdef WIN32
#include <winsock2.h> /* winsock.h is included automatically */
#include <process.h>
#include <io.h>
#define getopt getopt____
#else
#include <unistd.h>
#include <netinet/in.h>
#endif
#include <string.h>
#include <stdarg.h>
#include <search.h>
#include <pcap.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <assert.h>
#include <math.h>
#include "ndpi_api.h"
#include "uthash.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <libgen.h>
#include <sys/types.h>


#ifdef HAVE_JSON_C
#include <json.h>
#endif

#include "ndpi_util.h"
#include "queue.h"

#ifndef ETH_P_IP
#define ETH_P_IP               0x0800 	/* IPv4 */
#endif

#ifndef ETH_P_IPv6
#define ETH_P_IPV6	       0x86dd	/* IPv6 */
#endif

#define VLAN                   0x8100
#define MPLS_UNI               0x8847
#define MPLS_MULTI             0x8848
#define PPPoE                  0x8864
#define SNAP                   0xaa
#define BSTP                   0x42     /* Bridge Spanning Tree Protocol */

/* mask for FCF */
#define	WIFI_DATA                        0x2    /* 0000 0010 */
#define FCF_TYPE(fc)     (((fc) >> 2) & 0x3)    /* 0000 0011 = 0x3 */
#define FCF_SUBTYPE(fc)  (((fc) >> 4) & 0xF)    /* 0000 1111 = 0xF */
#define FCF_TO_DS(fc)        ((fc) & 0x0100)
#define FCF_FROM_DS(fc)      ((fc) & 0x0200)

/* mask for Bad FCF presence */
#define BAD_FCS                         0x50    /* 0101 0000 */

#define GTP_U_V1_PORT                   2152
#define TZSP_PORT                      37008

/** Client parameters **/
//  pthread_mutex_t lock;
//static int** packetDistributionArray;
u_char* sentPacketTemp;
static struct timeval startProg, endProg;
pthread_mutex_t** locks;
struct Node** threadQueueRears;
struct Node** threadQueueFronts;
static volatile int keepThreadsRunning = 1;
static int mbufInit = 1;
static int pauseDur = 10;
static int queueLength = 1000;
static struct rte_mempool *mbuf_pool;
static char *_pcap_file[MAX_NUM_READER_THREADS]; /**< Ingress pcap file/interfaces */
static FILE *playlist_fp[MAX_NUM_READER_THREADS] = { NULL }; /**< Ingress playlist */
static FILE *results_file           = NULL;
static char *results_path           = NULL;
static char * bpfFilter             = NULL; /**< bpf filter  */
static char *_protoFilePath         = NULL; /**< Protocol file path  */
static char *_customCategoryFilePath= NULL; /**< Custom categories file path  */
#ifdef HAVE_JSON_C
static char *_statsFilePath         = NULL; /**< Top stats file path */
static char *_diagnoseFilePath      = NULL; /**< Top stats file path */
static char *_jsonFilePath          = NULL; /**< JSON file path  */
static FILE *stats_fp               = NULL; /**< for Top Stats JSON file */
#endif
#ifdef HAVE_JSON_C
static json_object *jArray_known_flows, *jArray_unknown_flows;
static json_object *jArray_topStats;
#endif
static u_int8_t live_capture = 0;
static u_int8_t undetected_flows_deleted = 0;
/** User preferences **/
u_int8_t enable_protocol_guess = 1;
static u_int8_t verbose = 0, json_flag = 0;
int nDPI_LogLevel = 0;
char *_debug_protocols = NULL;
static u_int8_t stats_flag = 0, bpf_filter_flag = 0;
#ifdef HAVE_JSON_C
static u_int8_t file_first_time = 1;
#endif
static u_int32_t pcap_analysis_duration = (u_int32_t)-1;
static u_int16_t decode_tunnels = 0;
static u_int16_t num_loops = 1;
static u_int8_t shutdown_app = 0, quiet_mode = 0;
static u_int8_t num_threads = 1;
static u_int8_t num_pcap_files = 1;
static struct timeval startup_time, begin, end;
static struct timeval startSlice, endSlice;
#ifdef linux
static int core_affinity[MAX_NUM_READER_THREADS];
#endif
static struct timeval pcap_start = { 0, 0}, pcap_end = { 0, 0 };
/** Detection parameters **/
static time_t capture_for = 0;
static time_t capture_until = 0;
static u_int32_t num_flows;
static struct ndpi_detection_module_struct *ndpi_info_mod = NULL;

struct flow_info {
  struct ndpi_flow_info *flow;
  u_int16_t thread_id;
};

static struct flow_info *all_flows;

struct info_pair {
  u_int32_t addr;
  u_int8_t version; /* IP version */
  char proto[16]; /*app level protocol*/
  int count;
};

typedef struct node_a{
  u_int32_t addr;
  u_int8_t version; /* IP version */
  char proto[16]; /*app level protocol*/
  int count;
  struct node_a *left, *right;
}addr_node;

struct port_stats {
  u_int32_t port; /* we'll use this field as the key */
  u_int32_t num_pkts, num_bytes;
  u_int32_t num_flows;
  u_int32_t num_addr; /*number of distinct IP addresses */
  u_int32_t cumulative_addr; /*cumulative some of IP addresses */
  addr_node *addr_tree; /* tree of distinct IP addresses */
  struct info_pair top_ip_addrs[MAX_NUM_IP_ADDRESS];
  u_int8_t hasTopHost; /* as boolean flag */
  u_int32_t top_host;  /* host that is contributed to > 95% of traffic */
  u_int8_t version;    /* top host's ip version */
  char proto[16];      /* application level protocol of top host */
  UT_hash_handle hh;   /* makes this structure hashable */
};

struct port_stats *srcStats = NULL, *dstStats = NULL;

// struct to hold count of flows received by destination ports
struct port_flow_info {
  u_int32_t port; /* key */
  u_int32_t num_flows;
  UT_hash_handle hh;
};

// struct to hold single packet tcp flows sent by source ip address
struct single_flow_info {
  u_int32_t saddr; /* key */
  u_int8_t version; /* IP version */
  struct port_flow_info *ports;
  u_int32_t tot_flows;
  UT_hash_handle hh;
};

struct single_flow_info *scannerHosts = NULL;

// struct to hold top receiver hosts
struct receiver {
  u_int32_t addr; /* key */
  u_int8_t version; /* IP version */
  u_int32_t num_pkts;
  UT_hash_handle hh;
};

struct receiver *receivers = NULL, *topReceivers = NULL;

struct ndpi_packet_trailer {
  u_int32_t magic; /* 0x19682017 */
  u_int16_t master_protocol /* e.g. HTTP */, app_protocol /* e.g. FaceBook */;
  char name[16];
};

static pcap_dumper_t *extcap_dumper = NULL;
static char extcap_buf[16384];
static char *extcap_capture_fifo    = NULL;
static u_int16_t extcap_packet_filter = (u_int16_t)-1;

// struct associated to a workflow for a thread
struct reader_thread {
  struct ndpi_workflow *workflow;
  pthread_t pthread;
  u_int64_t last_idle_scan_time;
  u_int32_t idle_scan_idx;
  u_int32_t num_idle_flows;
  struct ndpi_flow_info *idle_flows[IDLE_SCAN_BUDGET];
};

// array for every thread created for a flow
static struct reader_thread ndpi_thread_info[MAX_NUM_READER_THREADS];

// ID tracking
typedef struct ndpi_id {
  u_int8_t ip[4];		   // Ip address
  struct ndpi_id_struct *ndpi_id;  // nDpi worker structure
} ndpi_id_t;

// used memory counters
u_int32_t current_ndpi_memory = 0, max_ndpi_memory = 0;
#ifdef USE_DPDK
static int dpdk_port_id = 0, dpdk_run_capture = 1;
#endif

void test_lib(); /* Forward */
void start_threads();
void * thread_waiting_loop(void *_thread_id);

/* ********************************** */

#ifdef DEBUG_TRACE
FILE *trace = NULL;
#endif

/********************** FUNCTIONS ********************* */

/**
 * @brief Set main components necessary to the detection
 */
static void setupDetection(u_int16_t thread_id, pcap_t * pcap_handle);

/**
 * @brief Print help instructions
 */
static void help(u_int long_help) {
  printf("Welcome to nDPI %s\n\n", ndpi_revision());

  printf("ndpiReader "
#ifndef USE_DPDK
	 "-i <file|device> "
#endif
	 "[-f <filter>][-s <duration>][-m <duration>]\n"
	 "          [-p <protos>][-l <loops> [-q][-d][-h][-t][-v <level>]\n"
	 "          [-n <threads>][-w <file>][-c <file>][-j <file>][-x <file>]\n\n"
	 "Usage:\n"
	 "  -i <file.pcap|device>     | Specify a pcap file/playlist to read packets from or a\n"
	 "                            | device for live capture (comma-separated list)\n"
	 "  -f <BPF filter>           | Specify a BPF filter for filtering selected traffic\n"
	 "  -s <duration>             | Maximum capture duration in seconds (live traffic capture only)\n"
	 "  -m <duration>             | Split analysis duration in <duration> max seconds\n"
	 "  -p <file>.protos          | Specify a protocol file (eg. protos.txt)\n"
	 "  -l <num loops>            | Number of detection loops (test only)\n"
	 "  -n <num threads>          | Number of threads. Default: number of interfaces in -i.\n"
	 "                            | Ignored with pcap files.\n"
	 "  -j <file.json>            | Specify a file to write the content of packets in .json format\n"
#ifdef linux
         "  -g <id:id...>             | Thread affinity mask (one core id per thread)\n"
#endif
	 "  -d                        | Disable protocol guess and use only DPI\n"
	 "  -q                        | Quiet mode\n"
	 "  -t                        | Dissect GTP/TZSP tunnels\n"
	 "  -r                        | Print nDPI version and git revision\n"
	 "  -c <path>                 | Load custom categories from the specified file\n"
	 "  -w <path>                 | Write test output on the specified file. This is useful for\n"
	 "                            | testing purposes in order to compare results across runs\n"
	 "  -h                        | This help\n"
	 "  -v <1|2|3>                | Verbose 'unknown protocol' packet print.\n"
	 "                            | 1 = verbose\n"
	 "                            | 2 = very verbose\n"
	 "                            | 3 = port stats\n"
	 "  -V <1-4>                  | nDPI logging level\n"
	 "                            | 1 - trace, 2 - debug, 3 - full debug\n"
	 "                            | >3 - full debug + dbg_proto = all\n"
	 "  -b <file.json>            | Specify a file to write port based diagnose statistics\n"
	 "  -x <file.json>            | Produce bpf filters for specified diagnose file. Use\n"
	 "                            | this option only for .json files generated with -b flag.\n");


#ifndef WIN32
  printf("\nExcap (wireshark) options:\n"
	 "  --extcap-interfaces\n"
	 "  --extcap-version\n"
	 "  --extcap-dlts\n"
	 "  --extcap-interface <name>\n"
	 "  --extcap-config\n"
	 "  --capture\n"
	 "  --extcap-capture-filter\n"
	 "  --fifo <path to file or pipe>\n"
	 "  --debug\n"
	 "  --dbg-proto proto|num[,...]\n"
    );
#endif

  if(long_help) {
    printf("\n\nSupported protocols:\n");
    num_threads = 1;
    ndpi_dump_protocols(ndpi_info_mod);
  }
  exit(!long_help);
}

static struct option longopts[] = {
  /* mandatory extcap options */
  { "extcap-interfaces", no_argument, NULL, '0'},
  { "extcap-version", optional_argument, NULL, '1'},
  { "extcap-dlts", no_argument, NULL, '2'},
  { "extcap-interface", required_argument, NULL, '3'},
  { "extcap-config", no_argument, NULL, '4'},
  { "capture", no_argument, NULL, '5'},
  { "extcap-capture-filter", required_argument, NULL, '6'},
  { "fifo", required_argument, NULL, '7'},
  { "debug", no_argument, NULL, '8'},
  { "dbg-proto", required_argument, NULL, 257},
  { "ndpi-proto-filter", required_argument, NULL, '9'},

  /* ndpiReader options */
  { "enable-protocol-guess", no_argument, NULL, 'd'},
  { "interface", required_argument, NULL, 'i'},
  { "filter", required_argument, NULL, 'f'},
  { "cpu-bind", required_argument, NULL, 'g'},
  { "loops", required_argument, NULL, 'l'},
  { "num-threads", required_argument, NULL, 'n'},

  { "protos", required_argument, NULL, 'p'},
  { "capture-duration", required_argument, NULL, 's'},
  { "decode-tunnels", no_argument, NULL, 't'},
  { "revision", no_argument, NULL, 'r'},
  { "verbose", no_argument, NULL, 'v'},
  { "version", no_argument, NULL, 'V'},
  { "help", no_argument, NULL, 'h'},
  { "json", required_argument, NULL, 'j'},
  { "result-path", required_argument, NULL, 'w'},
  { "quiet", no_argument, NULL, 'q'},

  {0, 0, 0, 0}
};

/* ********************************** */

void extcap_interfaces() {
  printf("extcap {version=%s}\n", ndpi_revision());
  printf("interface {value=ndpi}{display=nDPI interface}\n");
  exit(0);
}

/* ********************************** */

void extcap_dlts() {
  u_int dlts_number = DLT_EN10MB;
  printf("dlt {number=%u}{name=%s}{display=%s}\n", dlts_number, "ndpi", "nDPI Interface");
  exit(0);
}

/* ********************************** */

struct ndpi_proto_sorter {
  int id;
  char name[16];
};

/* ********************************** */

int cmpProto(const void *_a, const void *_b) {
  struct ndpi_proto_sorter *a = (struct ndpi_proto_sorter*)_a;
  struct ndpi_proto_sorter *b = (struct ndpi_proto_sorter*)_b;

  return(strcmp(a->name, b->name));
}

/* ********************************** */

int cmpFlows(const void *_a, const void *_b) {
  struct ndpi_flow_info *fa = ((struct flow_info*)_a)->flow;
  struct ndpi_flow_info *fb = ((struct flow_info*)_b)->flow;
  uint64_t a_size = fa->src2dst_bytes + fa->dst2src_bytes;
  uint64_t b_size = fb->src2dst_bytes + fb->dst2src_bytes;
  if(a_size != b_size)
    return a_size < b_size ? 1 : -1;

// copy from ndpi_workflow_node_cmp();

  if(fa->ip_version < fb->ip_version ) return(-1); else { if(fa->ip_version > fb->ip_version ) return(1); }
  if(fa->protocol   < fb->protocol   ) return(-1); else { if(fa->protocol   > fb->protocol   ) return(1); }
  if(htonl(fa->src_ip)   < htonl(fb->src_ip)  ) return(-1); else { if(htonl(fa->src_ip)   > htonl(fb->src_ip)  ) return(1); }
  if(htons(fa->src_port) < htons(fb->src_port)) return(-1); else { if(htons(fa->src_port) > htons(fb->src_port)) return(1); }
  if(htonl(fa->dst_ip)   < htonl(fb->dst_ip)  ) return(-1); else { if(htonl(fa->dst_ip)   > htonl(fb->dst_ip)  ) return(1); }
  if(htons(fa->dst_port) < htons(fb->dst_port)) return(-1); else { if(htons(fa->dst_port) > htons(fb->dst_port)) return(1); }
  return(0);
}

/* ********************************** */

void extcap_config() {
  int i, argidx = 0;
  struct ndpi_proto_sorter *protos;
  u_int ndpi_num_supported_protocols = ndpi_get_ndpi_num_supported_protocols(ndpi_info_mod);
  ndpi_proto_defaults_t *proto_defaults = ndpi_get_proto_defaults(ndpi_info_mod);

  /* -i <interface> */
  printf("arg {number=%d}{call=-i}{display=Capture Interface}{type=string}"
	 "{tooltip=The interface name}\n", argidx++);
  printf("arg {number=%d}{call=-i}{display=Pcap File to Analyze}{type=fileselect}"
	 "{tooltip=The pcap file to analyze (if the interface is unspecified)}\n", argidx++);

  protos = (struct ndpi_proto_sorter*)malloc(sizeof(struct ndpi_proto_sorter) * ndpi_num_supported_protocols);
  if(!protos) exit(0);

  for(i=0; i<(int) ndpi_num_supported_protocols; i++) {
    protos[i].id = i;
    snprintf(protos[i].name, sizeof(protos[i].name), "%s", proto_defaults[i].protoName);
  }

  qsort(protos, ndpi_num_supported_protocols, sizeof(struct ndpi_proto_sorter), cmpProto);

  printf("arg {number=%d}{call=-9}{display=nDPI Protocol Filter}{type=selector}"
	 "{tooltip=nDPI Protocol to be filtered}\n", argidx);

  printf("value {arg=%d}{value=%d}{display=%s}\n", argidx, -1, "All Protocols (no nDPI filtering)");

  for(i=0; i<(int)ndpi_num_supported_protocols; i++)
    printf("value {arg=%d}{value=%d}{display=%s (%d)}\n", argidx, protos[i].id,
	   protos[i].name, protos[i].id);

  free(protos);

  exit(0);
}

/* ********************************** */

void extcap_capture() {
#ifdef DEBUG_TRACE
  if(trace) fprintf(trace, " #### %s #### \n", __FUNCTION__);
#endif

  if((extcap_dumper = pcap_dump_open(pcap_open_dead(DLT_EN10MB, 16384 /* MTU */),
				     extcap_capture_fifo)) == NULL) {
    fprintf(stderr, "Unable to open the pcap dumper on %s", extcap_capture_fifo);

#ifdef DEBUG_TRACE
    if(trace) fprintf(trace, "Unable to open the pcap dumper on %s\n",
		      extcap_capture_fifo);
#endif
    return;
  }

#ifdef DEBUG_TRACE
  if(trace) fprintf(trace, "Starting packet capture [%p]\n", extcap_dumper);
#endif
}

/* ********************************** */

/**
 * @brief Option parser
 */
static void parseOptions(int argc, char **argv) {
  
  int option_idx = 0, do_capture = 0;
  char *__pcap_file = NULL, *bind_mask = NULL;
  int thread_id, opt;
#ifdef linux
  u_int num_cores = sysconf(_SC_NPROCESSORS_ONLN);
#endif

#ifdef DEBUG_TRACE
  trace = fopen("/tmp/ndpiReader.log", "a");

  if(trace) fprintf(trace, " #### %s #### \n", __FUNCTION__);
#endif

#ifdef USE_DPDK
  {
    int ret = rte_eal_init(argc, argv);

    if(ret < 0)
      rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");

    argc -= ret, argv += ret;
  }
#endif

  while((opt = getopt_long(argc, argv, "c:df:g:i:hp:l:s:tv:V:n:j:rp:w:q0123:456:7:89:m:b:x:", longopts, &option_idx)) != EOF) {
#ifdef DEBUG_TRACE
    if(trace) fprintf(trace, " #### -%c [%s] #### \n", opt, optarg ? optarg : "");
#endif

    switch (opt) {
    case 'd':
      enable_protocol_guess = 0;
      break;

    case 'i':
    case '3':
      _pcap_file[0] = optarg;
      break;

    case 'b':
#ifndef HAVE_JSON_C
      printf("WARNING: this copy of ndpiReader has been compiled without JSON-C: json export disabled\n");
#else
      _statsFilePath = optarg;
      stats_flag = 1;
#endif
      break;

    case 'm':
      pcap_analysis_duration = atol(optarg);
      break;

    case 'x':
#ifndef HAVE_JSON_C
      printf("WARNING: this copy of ndpiReader has been compiled without JSON-C: json export disabled\n");
#else
      _diagnoseFilePath = optarg;
      bpf_filter_flag = 1;
#endif
      break;

    case 'f':
    case '6':
      bpfFilter = optarg;
      break;

    case 'g':
      bind_mask = optarg;
      break;

    case 'l':
      num_loops = atoi(optarg);
      break;

    case 'n':
      num_threads = atoi(optarg);
      break;

    case 'p':
      _protoFilePath = optarg;
      break;

    case 'c':
      _customCategoryFilePath = optarg;
      break;

    case 's':
      capture_for = atoi(optarg);
      capture_until = capture_for + time(NULL);
      break;

    case 't':
      decode_tunnels = 1;
      break;

    case 'r':
      printf("ndpiReader - nDPI (%s)\n", ndpi_revision());
      exit(0);

    case 'v':
      verbose = atoi(optarg);
      break;

    case 'V':
      nDPI_LogLevel  = atoi(optarg);
      if(nDPI_LogLevel < 0) nDPI_LogLevel = 0;
      if(nDPI_LogLevel > 3) {
	nDPI_LogLevel = 3;
	_debug_protocols = strdup("all");
      }
      break;

    case 'h':
      help(1);
      break;

    case 'j':
#ifndef HAVE_JSON_C
      printf("WARNING: this copy of ndpiReader has been compiled without json-c: JSON export disabled\n");
#else
      _jsonFilePath = optarg;
      json_flag = 1;
#endif
      break;

    case 'w':
      results_path = strdup(optarg);
      if((results_file = fopen(results_path, "w")) == NULL) {
	printf("Unable to write in file %s: quitting\n", results_path);
	return;
      }
      break;

    case 'q':
      quiet_mode = 1;
      nDPI_LogLevel = 0;
      break;

      /* Extcap */
    case '0':
      extcap_interfaces();
      break;

    case '1':
      printf("extcap {version=%s}\n", ndpi_revision());
      break;

    case '2':
      extcap_dlts();
      break;

    case '4':
      extcap_config();
      break;

    case '5':
      do_capture = 1;
      break;

    case '7':
      extcap_capture_fifo = strdup(optarg);
      break;

    case '8':
      nDPI_LogLevel = NDPI_LOG_DEBUG_EXTRA;
      _debug_protocols = strdup("all");
      break;

    case '9':
      extcap_packet_filter = ndpi_get_proto_by_name(ndpi_info_mod, optarg);
      if(extcap_packet_filter == NDPI_PROTOCOL_UNKNOWN) extcap_packet_filter = atoi(optarg);
      break;

    case 257:
      _debug_protocols = strdup(optarg);
      break;

    default:
      help(0);
      break;
    }
  }

#ifndef USE_DPDK
  if(!bpf_filter_flag) {
    if(do_capture) {
      quiet_mode = 1;
      extcap_capture();
    }

    // check parameters
    if(!bpf_filter_flag && (_pcap_file[0] == NULL || strcmp(_pcap_file[0], "") == 0)) {
      help(0);
    }

    if(strchr(_pcap_file[0], ',')) { /* multiple ingress interfaces */
      num_pcap_files = 0;               /* setting number of threads = number of interfaces */
      __pcap_file = strtok(_pcap_file[0], ",");
      while(__pcap_file != NULL && num_pcap_files < MAX_NUM_READER_THREADS) {
        _pcap_file[num_pcap_files++] = __pcap_file;
        __pcap_file = strtok(NULL, ",");
      }
    } else {
      if(num_pcap_files > MAX_NUM_READER_THREADS) num_pcap_files = MAX_NUM_READER_THREADS;
      for(thread_id = 1; thread_id < num_pcap_files; thread_id++)
        _pcap_file[thread_id] = _pcap_file[0];
    }

#ifdef linux
    for(thread_id = 0; thread_id < num_pcap_files; thread_id++)
      core_affinity[thread_id] = -1;

    if(num_cores > 1 && bind_mask != NULL) {
      char *core_id = strtok(bind_mask, ":");
      thread_id = 0;
      while(core_id != NULL && thread_id < num_pcap_files) {
        core_affinity[thread_id++] = atoi(core_id) % num_cores;
        core_id = strtok(NULL, ":");
      }
    }
#endif
  }
#endif

#ifdef DEBUG_TRACE
  if(trace) fclose(trace);
#endif
}

/* ********************************** */

/**
 * @brief From IPPROTO to string NAME
 */
static char* ipProto2Name(u_int16_t proto_id) {
  static char proto[8];

  switch(proto_id) {
  case IPPROTO_TCP:
    return("TCP");
    break;
  case IPPROTO_UDP:
    return("UDP");
    break;
  case IPPROTO_ICMP:
    return("ICMP");
    break;
  case IPPROTO_ICMPV6:
    return("ICMPV6");
    break;
  case 112:
    return("VRRP");
    break;
  case IPPROTO_IGMP:
    return("IGMP");
    break;
  }

  snprintf(proto, sizeof(proto), "%u", proto_id);
  return(proto);
}

/* ********************************** */

/**
 * @brief A faster replacement for inet_ntoa().
 */
char* intoaV4(u_int32_t addr, char* buf, u_int16_t bufLen) {
  char *cp, *retStr;
  uint byte;
  int n;

  cp = &buf[bufLen];
  *--cp = '\0';

  n = 4;
  do {
    byte = addr & 0xff;
    *--cp = byte % 10 + '0';
    byte /= 10;
    if(byte > 0) {
      *--cp = byte % 10 + '0';
      byte /= 10;
      if(byte > 0)
	*--cp = byte + '0';
    }
    *--cp = '.';
    addr >>= 8;
  } while(--n > 0);

  /* Convert the string to lowercase */
  retStr = (char*)(cp+1);

  return(retStr);
}

/* ********************************** */

/**
 * @brief Print the flow
 */
static void printFlow(u_int16_t id, struct ndpi_flow_info *flow, u_int16_t thread_id) {
#ifdef HAVE_JSON_C
  json_object *jObj;
#endif
  FILE *out = results_file ? results_file : stdout;

  if((verbose != 1) && (verbose != 2))
    return;

  if(!json_flag) {
    fprintf(out, "\t%u", id);

    fprintf(out, "\t%s ", ipProto2Name(flow->protocol));

    fprintf(out, "%s%s%s:%u %s %s%s%s:%u ",
	    (flow->ip_version == 6) ? "[" : "",
	    flow->src_name, (flow->ip_version == 6) ? "]" : "", ntohs(flow->src_port),
	    flow->bidirectional ? "<->" : "->",
	    (flow->ip_version == 6) ? "[" : "",
	    flow->dst_name, (flow->ip_version == 6) ? "]" : "", ntohs(flow->dst_port)
      );

    if(flow->vlan_id > 0) fprintf(out, "[VLAN: %u]", flow->vlan_id);

    if(flow->detected_protocol.master_protocol) {
      char buf[64];

      fprintf(out, "[proto: %u.%u/%s]",
	      flow->detected_protocol.master_protocol, flow->detected_protocol.app_protocol,
	      ndpi_protocol2name(ndpi_thread_info[thread_id].workflow->ndpi_struct,
				 flow->detected_protocol, buf, sizeof(buf)));
    } else
      fprintf(out, "[proto: %u/%s]",
	      flow->detected_protocol.app_protocol,
	      ndpi_get_proto_name(ndpi_thread_info[thread_id].workflow->ndpi_struct, flow->detected_protocol.app_protocol));

    if(flow->detected_protocol.category != 0)
      fprintf(out, "[cat: %s/%u]",
	      ndpi_category_get_name(ndpi_thread_info[thread_id].workflow->ndpi_struct,
				     flow->detected_protocol.category),
	      (unsigned int)flow->detected_protocol.category);

    fprintf(out, "[%u pkts/%llu bytes ", flow->src2dst_packets, (long long unsigned int) flow->src2dst_bytes);
    fprintf(out, "%s %u pkts/%llu bytes]",
	    (flow->dst2src_packets > 0) ? "<->" : "->",
	    flow->dst2src_packets, (long long unsigned int) flow->dst2src_bytes);

    if(flow->host_server_name[0] != '\0') fprintf(out, "[Host: %s]", flow->host_server_name);
    if(flow->info[0] != '\0') fprintf(out, "[%s]", flow->info);

    if(flow->ssh_ssl.client_info[0] != '\0') fprintf(out, "[client: %s]", flow->ssh_ssl.client_info);
    if(flow->ssh_ssl.server_info[0] != '\0') fprintf(out, "[server: %s]", flow->ssh_ssl.server_info);
    if(flow->bittorent_hash[0] != '\0') fprintf(out, "[BT Hash: %s]", flow->bittorent_hash);

    fprintf(out, "\n");
  } else {
#ifdef HAVE_JSON_C
    jObj = json_object_new_object();

    json_object_object_add(jObj,"protocol",json_object_new_string(ipProto2Name(flow->protocol)));
    json_object_object_add(jObj,"host_a.name",json_object_new_string(flow->src_name));
    json_object_object_add(jObj,"host_a.port",json_object_new_int(ntohs(flow->src_port)));
    json_object_object_add(jObj,"host_b.name",json_object_new_string(flow->dst_name));
    json_object_object_add(jObj,"host_b.port",json_object_new_int(ntohs(flow->dst_port)));

    if(flow->detected_protocol.master_protocol)
      json_object_object_add(jObj,"detected.master_protocol",
			     json_object_new_int(flow->detected_protocol.master_protocol));

    json_object_object_add(jObj,"detected.app_protocol",
			   json_object_new_int(flow->detected_protocol.app_protocol));

    if(flow->detected_protocol.master_protocol) {
      char tmp[256];

      snprintf(tmp, sizeof(tmp), "%s.%s",
	       ndpi_get_proto_name(ndpi_thread_info[thread_id].workflow->ndpi_struct,
				   flow->detected_protocol.master_protocol),
	       ndpi_get_proto_name(ndpi_thread_info[thread_id].workflow->ndpi_struct,
				   flow->detected_protocol.app_protocol));

      json_object_object_add(jObj,"detected.protocol.name",
			     json_object_new_string(tmp));
    } else
      json_object_object_add(jObj,"detected.protocol.name",
			     json_object_new_string(ndpi_get_proto_name(ndpi_thread_info[thread_id].workflow->ndpi_struct,
									flow->detected_protocol.app_protocol)));

    json_object_object_add(jObj,"packets",json_object_new_int(flow->src2dst_packets + flow->dst2src_packets));
    json_object_object_add(jObj,"bytes",json_object_new_int(flow->src2dst_bytes + flow->dst2src_bytes));

    if(flow->host_server_name[0] != '\0')
      json_object_object_add(jObj,"host.server.name",json_object_new_string(flow->host_server_name));

    if((flow->ssh_ssl.client_info[0] != '\0') || (flow->ssh_ssl.server_info[0] != '\0')) {
      json_object *sjObj = json_object_new_object();

      if(flow->ssh_ssl.client_info[0] != '\0')
	json_object_object_add(sjObj, "client", json_object_new_string(flow->ssh_ssl.client_info));

      if(flow->ssh_ssl.server_info[0] != '\0')
	json_object_object_add(sjObj, "server", json_object_new_string(flow->ssh_ssl.server_info));

      json_object_object_add(jObj, "ssh_ssl", sjObj);
    }

    if(json_flag == 1)
      json_object_array_add(jArray_known_flows,jObj);
    else if(json_flag == 2)
      json_object_array_add(jArray_unknown_flows,jObj);
#endif
  }
}

/* ********************************** */

/**
 * @brief Unknown Proto Walker
 */
static void node_print_unknown_proto_walker(const void *node,
					    ndpi_VISIT which, int depth, void *user_data) {
  struct ndpi_flow_info *flow = *(struct ndpi_flow_info**)node;
  u_int16_t thread_id = *((u_int16_t*)user_data);

  if(flow->detected_protocol.app_protocol != NDPI_PROTOCOL_UNKNOWN) return;

  if((which == ndpi_preorder) || (which == ndpi_leaf)) {
    /* Avoid walking the same node multiple times */
    all_flows[num_flows].thread_id = thread_id, all_flows[num_flows].flow = flow;
    num_flows++;
  }
}

/* ********************************** */

/**
 * @brief Known Proto Walker
 */
static void node_print_known_proto_walker(const void *node,
					  ndpi_VISIT which, int depth, void *user_data) {
  struct ndpi_flow_info *flow = *(struct ndpi_flow_info**)node;
  u_int16_t thread_id = *((u_int16_t*)user_data);

  if(flow->detected_protocol.app_protocol == NDPI_PROTOCOL_UNKNOWN) return;

  if((which == ndpi_preorder) || (which == ndpi_leaf)) {
    /* Avoid walking the same node multiple times */
    all_flows[num_flows].thread_id = thread_id, all_flows[num_flows].flow = flow;
    num_flows++;
  }
}

/* ********************************** */

/**
 * @brief Proto Guess Walker
 */
static void node_proto_guess_walker(const void *node, ndpi_VISIT which, int depth, void *user_data) {
  struct ndpi_flow_info *flow = *(struct ndpi_flow_info **) node;
  u_int16_t thread_id = *((u_int16_t *) user_data);

  if((which == ndpi_preorder) || (which == ndpi_leaf)) { /* Avoid walking the same node multiple times */
    if((!flow->detection_completed) && flow->ndpi_flow)
      flow->detected_protocol = ndpi_detection_giveup(ndpi_thread_info[0].workflow->ndpi_struct, flow->ndpi_flow, enable_protocol_guess);

    process_ndpi_collected_info(ndpi_thread_info[thread_id].workflow, flow);

    ndpi_thread_info[thread_id].workflow->stats.protocol_counter[flow->detected_protocol.app_protocol]       += flow->src2dst_packets + flow->dst2src_packets;
    ndpi_thread_info[thread_id].workflow->stats.protocol_counter_bytes[flow->detected_protocol.app_protocol] += flow->src2dst_bytes + flow->dst2src_bytes;
    ndpi_thread_info[thread_id].workflow->stats.protocol_flows[flow->detected_protocol.app_protocol]++;
  }
}

/* *********************************************** */

void updateScanners(struct single_flow_info **scanners, u_int32_t saddr,
                    u_int8_t version, u_int32_t dport) {
  struct single_flow_info *f;
  struct port_flow_info *p;

  HASH_FIND_INT(*scanners, (int *)&saddr, f);

  if(f == NULL) {
    f = (struct single_flow_info*)malloc(sizeof(struct single_flow_info));
    if(!f) return;
    f->saddr = saddr;
    f->version = version;
    f->tot_flows = 1;
    f->ports = NULL;

    p = (struct port_flow_info*)malloc(sizeof(struct port_flow_info));

    if(!p) {
      free(f);
      return;
    } else
      p->port = dport, p->num_flows = 1;

    HASH_ADD_INT(f->ports, port, p);
    HASH_ADD_INT(*scanners, saddr, f);
  } else{
    struct port_flow_info *pp;
    f->tot_flows++;

    HASH_FIND_INT(f->ports, (int *)&dport, pp);

    if(pp == NULL) {
      pp = (struct port_flow_info*)malloc(sizeof(struct port_flow_info));
      if(!pp) return;
      pp->port = dport, pp->num_flows = 1;

      HASH_ADD_INT(f->ports, port, pp);
    } else
      pp->num_flows++;
  }
}

/* *********************************************** */

int updateIpTree(u_int32_t key, u_int8_t version,
		 addr_node **vrootp, const char *proto) {
  addr_node *q;
  addr_node **rootp = vrootp;

  if(rootp == (addr_node **)0)
    return 0;

  while(*rootp != (addr_node *)0) {
    /* Knuth's T1: */
    if((version == (*rootp)->version) && (key == (*rootp)->addr)) {
      /* T2: */
      return ++((*rootp)->count);
    }

    rootp = (key < (*rootp)->addr) ?
      &(*rootp)->left :		/* T3: follow left branch */
      &(*rootp)->right;		/* T4: follow right branch */
  }

  q = (addr_node *) malloc(sizeof(addr_node));	/* T5: key not found */
  if(q != (addr_node *)0) {	                /* make new node */
    *rootp = q;			                /* link new node to old */

    q->addr = key;
    q->version = version;
    strncpy(q->proto, proto, sizeof(q->proto));
    q->count = UPDATED_TREE;
    q->left = q->right = (addr_node *)0;

    return q->count;
  }

  return(0);
}
/* *********************************************** */

void freeIpTree(addr_node *root) {
  if(root == NULL)
    return;

  freeIpTree(root->left);
  freeIpTree(root->right);
  free(root);
  root = NULL;
}

/* *********************************************** */

void updateTopIpAddress(u_int32_t addr, u_int8_t version, const char *proto,
                        int count, struct info_pair top[], int size) {
  struct info_pair pair;
  int min = count;
  int update = 0;
  int min_i = 0;
  int i;

  if(count == 0) return;

  pair.addr = addr;
  pair.version = version;
  pair.count = count;
  strncpy(pair.proto, proto, sizeof(pair.proto));

  for(i=0; i<size; i++) {
    /* if the same ip with a bigger
       count just update it     */
    if(top[i].addr == addr) {
      top[i].count = count;
      return;
    }
    /* if array is not full yet
       add it to the first empty place */
    if(top[i].count == 0) {
      top[i] = pair;
      return;
    }
  }

  /* if bigger than the smallest one, replace it */
  for(i=0; i<size; i++) {
    if(top[i].count < count && top[i].count < min) {
      min = top[i].count;
      min_i = i;
      update = 1;
    }
  }

  if(update)
    top[min_i] = pair;
}

/* *********************************************** */

static void updatePortStats(struct port_stats **stats, u_int32_t port,
			    u_int32_t addr, u_int8_t version,
                            u_int32_t num_pkts, u_int32_t num_bytes,
                            const char *proto) {

  struct port_stats *s = NULL;
  int count = 0;

  HASH_FIND_INT(*stats, &port, s);
  if(s == NULL) {
    s = (struct port_stats*)calloc(1, sizeof(struct port_stats));
    if(!s) return;

    s->port = port, s->num_pkts = num_pkts, s->num_bytes = num_bytes;
    s->num_addr = 1, s->cumulative_addr = 1; s->num_flows = 1;

    updateTopIpAddress(addr, version, proto, 1, s->top_ip_addrs, MAX_NUM_IP_ADDRESS);

    s->addr_tree = (addr_node *) malloc(sizeof(addr_node));
    if(!s->addr_tree) {
      free(s);
      return;
    }

    s->addr_tree->addr = addr;
    s->addr_tree->version = version;
    strncpy(s->addr_tree->proto, proto, sizeof(s->addr_tree->proto));
    s->addr_tree->count = 1;
    s->addr_tree->left = NULL;
    s->addr_tree->right = NULL;

    HASH_ADD_INT(*stats, port, s);
  }
  else{
    count = updateIpTree(addr, version, &(*s).addr_tree, proto);

    if(count == UPDATED_TREE) s->num_addr++;

    if(count) {
      s->cumulative_addr++;
      updateTopIpAddress(addr, version, proto, count, s->top_ip_addrs, MAX_NUM_IP_ADDRESS);
    }

    s->num_pkts += num_pkts, s->num_bytes += num_bytes, s->num_flows++;
  }
}

/* *********************************************** */

/* @brief heuristic choice for receiver stats */
static int acceptable(u_int32_t num_pkts){
  return num_pkts > 5;
}

/* *********************************************** */

static int receivers_sort(void *_a, void *_b) {
  struct receiver *a = (struct receiver *)_a;
  struct receiver *b = (struct receiver *)_b;

  return(b->num_pkts - a->num_pkts);
}

/* *********************************************** */

static int receivers_sort_asc(void *_a, void *_b) {
  struct receiver *a = (struct receiver *)_a;
  struct receiver *b = (struct receiver *)_b;

  return(a->num_pkts - b->num_pkts);
}

/* ***************************************************** */
/*@brief removes first (size - max) elements from hash table.
 * hash table is ordered in ascending order.
 */
static struct receiver *cutBackTo(struct receiver **receivers, u_int32_t size, u_int32_t max) {
  struct receiver *r, *tmp;
  int i=0;
  int count;

  if(size < max) //return the original table
    return *receivers;

  count = size - max;

  HASH_ITER(hh, *receivers, r, tmp) {
    if(i++ == count)
      return r;
    HASH_DEL(*receivers, r);
    free(r);
  }

  return(NULL);

}

/* *********************************************** */
/*@brief merge first table to the second table.
 * if element already in the second table
 *  then updates its value
 * else adds it to the second table
 */
static void mergeTables(struct receiver **primary, struct receiver **secondary) {
  struct receiver *r, *s, *tmp;

  HASH_ITER(hh, *primary, r, tmp) {
    HASH_FIND_INT(*secondary, (int *)&(r->addr), s);
    if(s == NULL){
      s = (struct receiver *)malloc(sizeof(struct receiver));
      if(!s) return;

      s->addr = r->addr;
      s->version = r->version;
      s->num_pkts = r->num_pkts;

      HASH_ADD_INT(*secondary, addr, s);
    }
    else
      s->num_pkts += r->num_pkts;

    HASH_DEL(*primary, r);
    free(r);
  }
}
/* *********************************************** */

static void deleteReceivers(struct receiver *receivers) {
  struct receiver *current, *tmp;

  HASH_ITER(hh, receivers, current, tmp) {
    HASH_DEL(receivers, current);
    free(current);
  }
}

/* *********************************************** */
/* implementation of: https://jeroen.massar.ch/presentations/files/FloCon2010-TopK.pdf
 *
 * if(table1.size < max1 || acceptable){
 *    create new element and add to the table1
 *    if(table1.size > max2) {
 *      cut table1 back to max1
 *      merge table 1 to table2
 *      if(table2.size > max1)
 *        cut table2 back to max1
 *    }
 * }
 * else
 *   update table1
 */
static void updateReceivers(struct receiver **receivers, u_int32_t dst_addr,
                            u_int8_t version, u_int32_t num_pkts,
                            struct receiver **topReceivers) {
  struct receiver *r;
  u_int32_t size;
  int a;

  HASH_FIND_INT(*receivers, (int *)&dst_addr, r);
  if(r == NULL) {
    if(((size = HASH_COUNT(*receivers)) < MAX_TABLE_SIZE_1)
       || ((a = acceptable(num_pkts)) != 0)){
      r = (struct receiver *)malloc(sizeof(struct receiver));
      if(!r) return;

      r->addr = dst_addr;
      r->version = version;
      r->num_pkts = num_pkts;

      HASH_ADD_INT(*receivers, addr, r);

      if((size = HASH_COUNT(*receivers)) > MAX_TABLE_SIZE_2){

        HASH_SORT(*receivers, receivers_sort_asc);
        *receivers = cutBackTo(receivers, size, MAX_TABLE_SIZE_1);
        mergeTables(receivers, topReceivers);

        if((size = HASH_COUNT(*topReceivers)) > MAX_TABLE_SIZE_1){
          HASH_SORT(*topReceivers, receivers_sort_asc);
          *topReceivers = cutBackTo(topReceivers, size, MAX_TABLE_SIZE_1);
        }

        *receivers = NULL;
      }
    }
  }
  else
    r->num_pkts += num_pkts;
}

/* *********************************************** */

#ifdef HAVE_JSON_C
static void saveReceiverStats(json_object **jObj_group,
                              struct receiver **receivers,
                              u_int64_t total_pkt_count) {

  json_object *jArray_stats  = json_object_new_array();
  struct receiver *r, *tmp;
  int i = 0;

  HASH_ITER(hh, *receivers, r, tmp) {
    json_object *jObj_stat = json_object_new_object();
    char addr_name[48];

    if(r->version == IPVERSION)
      inet_ntop(AF_INET, &(r->addr), addr_name, sizeof(addr_name));
    else
      inet_ntop(AF_INET6, &(r->addr),  addr_name, sizeof(addr_name));


    json_object_object_add(jObj_stat,"ip.address",json_object_new_string(addr_name));
    json_object_object_add(jObj_stat,"packets.number", json_object_new_int(r->num_pkts));
    json_object_object_add(jObj_stat,"packets.percent",json_object_new_double(((double)r->num_pkts) / total_pkt_count));

    json_object_array_add(jArray_stats, jObj_stat);

    i++;
    if(i >= 10) break;
  }

  json_object_object_add(*jObj_group, "top.receiver.stats", jArray_stats);
}
#endif


/* *********************************************** */

static void deleteScanners(struct single_flow_info *scanners) {
  struct single_flow_info *s, *tmp;
  struct port_flow_info *p, *tmp2;

  HASH_ITER(hh, scanners, s, tmp) {
    HASH_ITER(hh, s->ports, p, tmp2) {
      HASH_DEL(s->ports, p);
      free(p);
    }
    HASH_DEL(scanners, s);
    free(s);
  }
}

/* *********************************************** */

static void deletePortsStats(struct port_stats *stats) {
  struct port_stats *current_port, *tmp;

  HASH_ITER(hh, stats, current_port, tmp) {
    HASH_DEL(stats, current_port);
    freeIpTree(current_port->addr_tree);
    free(current_port);
  }
}

/* *********************************************** */

/**
 * @brief Ports stats
 */
static void port_stats_walker(const void *node, ndpi_VISIT which, int depth, void *user_data) {
  if((which == ndpi_preorder) || (which == ndpi_leaf)) { /* Avoid walking the same node multiple times */
    struct ndpi_flow_info *flow = *(struct ndpi_flow_info **) node;
    u_int16_t thread_id = *(int *)user_data;
    u_int16_t sport, dport;
    char proto[16];
    int r;

    sport = ntohs(flow->src_port), dport = ntohs(flow->dst_port);

    /* get app level protocol */
    if(flow->detected_protocol.master_protocol)
      ndpi_protocol2name(ndpi_thread_info[thread_id].workflow->ndpi_struct,
			 flow->detected_protocol, proto, sizeof(proto));
    else
      strncpy(proto, ndpi_get_proto_name(ndpi_thread_info[thread_id].workflow->ndpi_struct,
					 flow->detected_protocol.app_protocol),sizeof(proto));

    if(((r = strcmp(ipProto2Name(flow->protocol), "TCP")) == 0)
       && (flow->src2dst_packets == 1) && (flow->dst2src_packets == 0)) {
      updateScanners(&scannerHosts, flow->src_ip, flow->ip_version, dport);
    }

    updateReceivers(&receivers, flow->dst_ip, flow->ip_version,
                    flow->src2dst_packets, &topReceivers);

    updatePortStats(&srcStats, sport, flow->src_ip, flow->ip_version,
                    flow->src2dst_packets, flow->src2dst_bytes, proto);

    updatePortStats(&dstStats, dport, flow->dst_ip, flow->ip_version,
                    flow->dst2src_packets, flow->dst2src_bytes, proto);
  }
}

/* *********************************************** */

/**
 * @brief Idle Scan Walker
 */
static void node_idle_scan_walker(const void *node, ndpi_VISIT which, int depth, void *user_data) {
  struct ndpi_flow_info *flow = *(struct ndpi_flow_info **) node;
  u_int16_t thread_id = *((u_int16_t *) user_data);

  if(ndpi_thread_info[thread_id].num_idle_flows == IDLE_SCAN_BUDGET) /* TODO optimise with a budget-based walk */
    return;

  if((which == ndpi_preorder) || (which == ndpi_leaf)) { /* Avoid walking the same node multiple times */
    if(flow->last_seen + MAX_IDLE_TIME < ndpi_thread_info[thread_id].workflow->last_time) {

      /* update stats */
      node_proto_guess_walker(node, which, depth, user_data);

      if((flow->detected_protocol.app_protocol == NDPI_PROTOCOL_UNKNOWN) && !undetected_flows_deleted)
        undetected_flows_deleted = 1;

      ndpi_free_flow_info_half(flow);
      ndpi_thread_info[thread_id].workflow->stats.ndpi_flow_count--;

      /* adding to a queue (we can't delete it from the tree inline ) */
      ndpi_thread_info[thread_id].idle_flows[ndpi_thread_info[thread_id].num_idle_flows++] = flow;
    }
  }
}


/**
 * @brief On Protocol Discover - demo callback
 */
static void on_protocol_discovered(struct ndpi_workflow * workflow,
				   struct ndpi_flow_info * flow,
				   void * udata) {
  ;
}

#if 0
/**
 * @brief Print debug
 */
static void debug_printf(u_int32_t protocol, void *id_struct,
			 ndpi_log_level_t log_level,
			 const char *format, ...) {

  va_list va_ap;
#ifndef WIN32
  struct tm result;
#endif

  if(log_level <= nDPI_LogLevel) {
    char buf[8192], out_buf[8192];
    char theDate[32];
    const char *extra_msg = "";
    time_t theTime = time(NULL);

    va_start (va_ap, format);

    if(log_level == NDPI_LOG_ERROR)
      extra_msg = "ERROR: ";
    else if(log_level == NDPI_LOG_TRACE)
      extra_msg = "TRACE: ";
    else
      extra_msg = "DEBUG: ";

    memset(buf, 0, sizeof(buf));
    strftime(theDate, 32, "%d/%b/%Y %H:%M:%S", localtime_r(&theTime,&result) );
    vsnprintf(buf, sizeof(buf)-1, format, va_ap);

    snprintf(out_buf, sizeof(out_buf), "%s %s%s", theDate, extra_msg, buf);
    printf("%s", out_buf);
    fflush(stdout);
  }

  va_end(va_ap);
}
#endif

/**
 * @brief Setup for detection begin
 */
static void setupDetection(u_int16_t thread_id, pcap_t * pcap_handle) {
  NDPI_PROTOCOL_BITMASK all;
  struct ndpi_workflow_prefs prefs;
  
  memset(&prefs, 0, sizeof(prefs));
  prefs.decode_tunnels = decode_tunnels;
  prefs.num_roots = NUM_ROOTS;
  prefs.max_ndpi_flows = MAX_NDPI_FLOWS;
  prefs.quiet_mode = quiet_mode;

  pthread_t pthreadCopy = ndpi_thread_info[thread_id].pthread;
  memset(&ndpi_thread_info[thread_id], 0, sizeof(ndpi_thread_info[thread_id]));
  ndpi_thread_info[thread_id].pthread = pthreadCopy;
  ndpi_thread_info[thread_id].workflow = ndpi_workflow_init(&prefs, pcap_handle);
  
  /* Preferences */
  ndpi_set_detection_preferences(ndpi_thread_info[thread_id].workflow->ndpi_struct,
				 ndpi_pref_http_dont_dissect_response, 0);
  ndpi_set_detection_preferences(ndpi_thread_info[thread_id].workflow->ndpi_struct,
				 ndpi_pref_dns_dissect_response, 0);
  ndpi_set_detection_preferences(ndpi_thread_info[thread_id].workflow->ndpi_struct,
				 ndpi_pref_enable_category_substring_match, 1);

  ndpi_workflow_set_flow_detected_callback(ndpi_thread_info[thread_id].workflow,
					   on_protocol_discovered,
					   (void *)(uintptr_t)thread_id);

  // enable all protocols
  NDPI_BITMASK_SET_ALL(all);
  ndpi_set_protocol_detection_bitmask2(ndpi_thread_info[thread_id].workflow->ndpi_struct, &all);
  // clear memory for results
  memset(ndpi_thread_info[thread_id].workflow->stats.protocol_counter, 0,
	 sizeof(ndpi_thread_info[thread_id].workflow->stats.protocol_counter));
  memset(ndpi_thread_info[thread_id].workflow->stats.protocol_counter_bytes, 0,
	 sizeof(ndpi_thread_info[thread_id].workflow->stats.protocol_counter_bytes));
  memset(ndpi_thread_info[thread_id].workflow->stats.protocol_flows, 0,
	 sizeof(ndpi_thread_info[thread_id].workflow->stats.protocol_flows));

  if(_protoFilePath != NULL)
    ndpi_load_protocols_file(ndpi_thread_info[thread_id].workflow->ndpi_struct, _protoFilePath);

  if(_customCategoryFilePath) {
    FILE *fd = fopen(_customCategoryFilePath, "r");

    if(fd) {
      while(fd) {
	char buffer[512], *line, *name, *category;
	int i;

	if(!(line = fgets(buffer, sizeof(buffer), fd)))
	  break;

	if(((i = strlen(line)) <= 1) || (line[0] == '#'))
	  continue;
	else
	  line[i-1] = '\0';

	name = strtok(line, "\t");
	if(name) {
	  category = strtok(NULL, "\t");

	  if(category) {
	    int fields[4];

	    if(sscanf(name, "%d.%d.%d.%d", &fields[0], &fields[1], &fields[2], &fields[3]) == 4)
	      ndpi_load_ip_category(ndpi_thread_info[thread_id].workflow->ndpi_struct,
				    name, (ndpi_protocol_category_t)atoi(category));
	    else
	      ndpi_load_hostname_category(ndpi_thread_info[thread_id].workflow->ndpi_struct,
					  name, (ndpi_protocol_category_t)atoi(category));
	  }
	}
      }

      ndpi_enable_loaded_categories(ndpi_thread_info[thread_id].workflow->ndpi_struct);
    } else
      printf("ERROR: Unable to read file %s\n", _customCategoryFilePath);
  }
}


/**
 * @brief End of detection and free flow
 */
static void terminateDetection(u_int16_t thread_id) {
  ndpi_workflow_free(ndpi_thread_info[thread_id].workflow);
}


/**
 * @brief Traffic stats format
 */
char* formatTraffic(float numBits, int bits, char *buf) {

  char unit;

  if(bits)
    unit = 'b';
  else
    unit = 'B';

  if(numBits < 1024) {
    snprintf(buf, 32, "%lu %c", (unsigned long)numBits, unit);
  } else if(numBits < (1024*1024)) {
    snprintf(buf, 32, "%.2f K%c", (float)(numBits)/1024, unit);
  } else {
    float tmpMBits = ((float)numBits)/(1024*1024);

    if(tmpMBits < 1024) {
      snprintf(buf, 32, "%.2f M%c", tmpMBits, unit);
    } else {
      tmpMBits /= 1024;

      if(tmpMBits < 1024) {
	snprintf(buf, 32, "%.2f G%c", tmpMBits, unit);
      } else {
	snprintf(buf, 32, "%.2f T%c", (float)(tmpMBits)/1024, unit);
      }
    }
  }

  return(buf);
}


/**
 * @brief Packets stats format
 */
char* formatPackets(float numPkts, char *buf) {

  if(numPkts < 1000) {
    snprintf(buf, 32, "%.2f", numPkts);
  } else if(numPkts < (1000*1000)) {
    snprintf(buf, 32, "%.2f K", numPkts/1000);
  } else {
    numPkts /= (1000*1000);
    snprintf(buf, 32, "%.2f M", numPkts);
  }

  return(buf);
}


/**
 * @brief JSON function init
 */
#ifdef HAVE_JSON_C
static void json_init() {
  jArray_known_flows = json_object_new_array();
  jArray_unknown_flows = json_object_new_array();
  jArray_topStats = json_object_new_array();
}

static void json_open_stats_file() {
  if((file_first_time && ((stats_fp = fopen(_statsFilePath,"w")) == NULL))
     ||
     (!file_first_time && (stats_fp = fopen(_statsFilePath,"a")) == NULL)) {
    printf("Error creating/opening file %s\n", _statsFilePath);
    stats_flag = 0;
  }
  else file_first_time = 0;
}

static void json_close_stats_file() {
  json_object *jObjFinal = json_object_new_object();

  json_object_object_add(jObjFinal,"duration.in.seconds",
			 json_object_new_int(pcap_analysis_duration));
  json_object_object_add(jObjFinal,"statistics", jArray_topStats);
  fprintf(stats_fp,"%s\n",json_object_to_json_string(jObjFinal));
  fclose(stats_fp);
  json_object_put(jObjFinal);
}
#endif

/* *********************************************** */

/**
 * @brief Bytes stats format
 */
char* formatBytes(u_int32_t howMuch, char *buf, u_int buf_len) {
  char unit = 'B';

  if(howMuch < 1024) {
    snprintf(buf, buf_len, "%lu %c", (unsigned long)howMuch, unit);
  } else if(howMuch < (1024*1024)) {
    snprintf(buf, buf_len, "%.2f K%c", (float)(howMuch)/1024, unit);
  } else {
    float tmpGB = ((float)howMuch)/(1024*1024);

    if(tmpGB < 1024) {
      snprintf(buf, buf_len, "%.2f M%c", tmpGB, unit);
    } else {
      tmpGB /= 1024;

      snprintf(buf, buf_len, "%.2f G%c", tmpGB, unit);
    }
  }

  return(buf);
}

/* *********************************************** */

static int port_stats_sort(void *_a, void *_b) {
  struct port_stats *a = (struct port_stats*)_a;
  struct port_stats *b = (struct port_stats*)_b;

  if(b->num_pkts == 0 && a->num_pkts == 0)
    return(b->num_flows - a->num_flows);

  return(b->num_pkts - a->num_pkts);
}

/* *********************************************** */

#ifdef HAVE_JSON_C
static int scanners_sort(void *_a, void *_b) {
  struct single_flow_info *a = (struct single_flow_info *)_a;
  struct single_flow_info *b = (struct single_flow_info *)_b;

  return(b->tot_flows - a->tot_flows);
}

/* *********************************************** */

static int scanners_port_sort(void *_a, void *_b) {
  struct port_flow_info *a = (struct port_flow_info *)_a;
  struct port_flow_info *b = (struct port_flow_info *)_b;

  return(b->num_flows - a->num_flows);
}

#endif
/* *********************************************** */

static int info_pair_cmp (const void *_a, const void *_b)
{
  struct info_pair *a = (struct info_pair *)_a;
  struct info_pair *b = (struct info_pair *)_b;

  return b->count - a->count;
}

/* *********************************************** */

#ifdef HAVE_JSON_C
static int top_stats_sort(void *_a, void *_b) {
  struct port_stats *a = (struct port_stats*)_a;
  struct port_stats *b = (struct port_stats*)_b;

  return(b->num_addr - a->num_addr);
}

/* *********************************************** */

/**
 * @brief Get port based top statistics
 */
static int getTopStats(struct port_stats *stats) {
  struct port_stats *sp, *tmp;
  struct info_pair inf;
  u_int64_t total_ip_addrs = 0;

  HASH_ITER(hh, stats, sp, tmp) {
    qsort(sp->top_ip_addrs, MAX_NUM_IP_ADDRESS, sizeof(struct info_pair), info_pair_cmp);
    inf = sp->top_ip_addrs[0];

    if(((inf.count * 100.0)/sp->cumulative_addr) > AGGRESSIVE_PERCENT) {
      sp->hasTopHost = 1;
      sp->top_host = inf.addr;
      sp->version = inf.version;
      strncpy(sp->proto, inf.proto, sizeof(sp->proto));
    } else
      sp->hasTopHost = 0;

    total_ip_addrs += sp->num_addr;
  }

  return total_ip_addrs;
}

/* *********************************************** */

static void saveScannerStats(json_object **jObj_group, struct single_flow_info **scanners) {
  struct single_flow_info *s, *tmp;
  struct port_flow_info *p, *tmp2;
  char addr_name[48];
  int i = 0, j = 0;

  json_object *jArray_stats  = json_object_new_array();

  HASH_SORT(*scanners, scanners_sort); // FIX

  HASH_ITER(hh, *scanners, s, tmp) {
    json_object *jObj_stat = json_object_new_object();
    json_object *jArray_ports = json_object_new_array();

    if(s->version == IPVERSION)
      inet_ntop(AF_INET, &(s->saddr), addr_name, sizeof(addr_name));
    else
      inet_ntop(AF_INET6, &(s->saddr),  addr_name, sizeof(addr_name));

    json_object_object_add(jObj_stat,"ip.address",json_object_new_string(addr_name));
    json_object_object_add(jObj_stat,"total.flows.number",json_object_new_int(s->tot_flows));

    HASH_SORT(s->ports, scanners_port_sort);

    HASH_ITER(hh, s->ports, p, tmp2) {
      json_object *jObj_port = json_object_new_object();

      json_object_object_add(jObj_port,"port",json_object_new_int(p->port));
      json_object_object_add(jObj_port,"flows.number",json_object_new_int(p->num_flows));

      json_object_array_add(jArray_ports, jObj_port);

      j++;
      if(j >= 10) break;
    }

    json_object_object_add(jObj_stat,"top.dst.ports",jArray_ports);
    json_object_array_add(jArray_stats, jObj_stat);

    j = 0;
    i++;
    if(i >= 10) break;
  }

  json_object_object_add(*jObj_group, "top.scanner.stats", jArray_stats);
}

#endif

/* *********************************************** */

#ifdef HAVE_JSON_C
/*
 * @brief Save Top Stats in json format
 */
static void saveTopStats(json_object **jObj_group,
                         struct port_stats **stats,
                         u_int8_t direction,
                         u_int64_t total_flow_count,
                         u_int64_t total_ip_addr) {
  struct port_stats *s, *tmp;
  char addr_name[48];
  int i = 0;

  json_object *jArray_stats  = json_object_new_array();


  HASH_ITER(hh, *stats, s, tmp) {

    if((s->hasTopHost)) {
      json_object *jObj_stat = json_object_new_object();

      json_object_object_add(jObj_stat,"port",json_object_new_int(s->port));
      json_object_object_add(jObj_stat,"packets.number",json_object_new_int(s->num_pkts));
      json_object_object_add(jObj_stat,"flows.number",json_object_new_int(s->num_flows));
      json_object_object_add(jObj_stat,"flows.percent",json_object_new_double((s->num_flows*100.0)/total_flow_count));
      if(s->num_pkts) json_object_object_add(jObj_stat,"flows/packets",
					     json_object_new_double(((double)s->num_flows)/s->num_pkts));
      else json_object_object_add(jObj_stat,"flows.num_packets",json_object_new_double(0.0));

      if(s->version == IPVERSION) {
	inet_ntop(AF_INET, &(s->top_host), addr_name, sizeof(addr_name));
      } else {
	inet_ntop(AF_INET6, &(s->top_host),  addr_name, sizeof(addr_name));
      }

      json_object_object_add(jObj_stat,"aggressive.host",json_object_new_string(addr_name));
      json_object_object_add(jObj_stat,"host.app.protocol",json_object_new_string(s->proto));

      json_object_array_add(jArray_stats, jObj_stat);
      i++;

      if(i >= 10) break;
    }
  }

  json_object_object_add(*jObj_group, (direction == DIR_SRC) ?
			 "top.src.pkts.stats" : "top.dst.pkts.stats", jArray_stats);

  jArray_stats  = json_object_new_array();
  i=0;

  /*sort top stats by ip addr count*/
  HASH_SORT(*stats, top_stats_sort);


  HASH_ITER(hh, *stats, s, tmp) {
    json_object *jObj_stat = json_object_new_object();

    json_object_object_add(jObj_stat,"port",json_object_new_int(s->port));
    json_object_object_add(jObj_stat,"host.number",json_object_new_int64(s->num_addr));
    json_object_object_add(jObj_stat,"host.percent",json_object_new_double((s->num_addr*100.0)/total_ip_addr));
    json_object_object_add(jObj_stat,"flows.number",json_object_new_int(s->num_flows));

    json_object_array_add(jArray_stats,jObj_stat);
    i++;

    if(i >= 10) break;
  }

  json_object_object_add(*jObj_group, (direction == DIR_SRC) ?
			 "top.src.host.stats" : "top.dst.host.stats", jArray_stats);
}
#endif

/* *********************************************** */

void printPortStats(struct port_stats *stats) {
  struct port_stats *s, *tmp;
  char addr_name[48];
  int i = 0, j = 0;

  HASH_ITER(hh, stats, s, tmp) {
    i++;
    printf("\t%2d\tPort %5u\t[%u IP address(es)/%u flows/%u pkts/%u bytes]\n\t\tTop IP Stats:\n",
	   i, s->port, s->num_addr, s->num_flows, s->num_pkts, s->num_bytes);

    qsort(&s->top_ip_addrs[0], MAX_NUM_IP_ADDRESS, sizeof(struct info_pair), info_pair_cmp);

    for(j=0; j<MAX_NUM_IP_ADDRESS; j++) {
      if(s->top_ip_addrs[j].count != 0) {
        if(s->top_ip_addrs[j].version == IPVERSION) {
          inet_ntop(AF_INET, &(s->top_ip_addrs[j].addr), addr_name, sizeof(addr_name));
        } else {
          inet_ntop(AF_INET6, &(s->top_ip_addrs[j].addr),  addr_name, sizeof(addr_name));
        }

	printf("\t\t%-36s ~ %.2f%%\n", addr_name,
	       ((s->top_ip_addrs[j].count) * 100.0) / s->cumulative_addr);
      }
    }

    printf("\n");
    if(i >= 10) break;
  }
}


/* *********************************************** */

/**
 * @brief Print result
 */
static void printResults(u_int64_t processing_time_usec, u_int64_t setup_time_usec) {
  u_int32_t i;
  u_int64_t total_flow_bytes = 0;
  u_int32_t avg_pkt_size = 0;
  struct ndpi_stats cumulative_stats;
  int thread_id;
  char buf[32];
#ifdef HAVE_JSON_C
  FILE *json_fp = NULL;
  u_int8_t dont_close_json_fp = 0;
  json_object *jObj_main = NULL, *jObj_trafficStats, *jArray_detProto = NULL, *jObj;
#endif
  long long unsigned int breed_stats[NUM_BREEDS] = { 0 };

  memset(&cumulative_stats, 0, sizeof(cumulative_stats));
  for(thread_id = 0; thread_id < num_threads; thread_id++) {
    if((ndpi_thread_info[thread_id].workflow->stats.total_wire_bytes == 0)
       && (ndpi_thread_info[thread_id].workflow->stats.raw_packet_count == 0))
      continue;
    for(i=0; i<NUM_ROOTS; i++) {
      ndpi_twalk(ndpi_thread_info[thread_id].workflow->ndpi_flows_root[i],
		 node_proto_guess_walker, &thread_id);
      if(verbose == 3 || stats_flag) ndpi_twalk(ndpi_thread_info[thread_id].workflow->ndpi_flows_root[i],
						port_stats_walker, &thread_id);
    }
    /* Stats aggregation */
    cumulative_stats.guessed_flow_protocols += ndpi_thread_info[thread_id].workflow->stats.guessed_flow_protocols;
    cumulative_stats.raw_packet_count += ndpi_thread_info[thread_id].workflow->stats.raw_packet_count;
    cumulative_stats.ip_packet_count += ndpi_thread_info[thread_id].workflow->stats.ip_packet_count;
    cumulative_stats.total_wire_bytes += ndpi_thread_info[thread_id].workflow->stats.total_wire_bytes;
    cumulative_stats.total_ip_bytes += ndpi_thread_info[thread_id].workflow->stats.total_ip_bytes;
    cumulative_stats.total_discarded_bytes += ndpi_thread_info[thread_id].workflow->stats.total_discarded_bytes;

    for(i = 0; i < ndpi_get_num_supported_protocols(ndpi_thread_info[0].workflow->ndpi_struct); i++) {
      cumulative_stats.protocol_counter[i] += ndpi_thread_info[thread_id].workflow->stats.protocol_counter[i];
      cumulative_stats.protocol_counter_bytes[i] += ndpi_thread_info[thread_id].workflow->stats.protocol_counter_bytes[i];
      cumulative_stats.protocol_flows[i] += ndpi_thread_info[thread_id].workflow->stats.protocol_flows[i];
    }

    cumulative_stats.ndpi_flow_count += ndpi_thread_info[thread_id].workflow->stats.ndpi_flow_count;
    cumulative_stats.tcp_count   += ndpi_thread_info[thread_id].workflow->stats.tcp_count;
    cumulative_stats.udp_count   += ndpi_thread_info[thread_id].workflow->stats.udp_count;
    cumulative_stats.mpls_count  += ndpi_thread_info[thread_id].workflow->stats.mpls_count;
    cumulative_stats.pppoe_count += ndpi_thread_info[thread_id].workflow->stats.pppoe_count;
    cumulative_stats.vlan_count  += ndpi_thread_info[thread_id].workflow->stats.vlan_count;
    cumulative_stats.fragmented_count += ndpi_thread_info[thread_id].workflow->stats.fragmented_count;
    for(i = 0; i < sizeof(cumulative_stats.packet_len)/sizeof(cumulative_stats.packet_len[0]); i++)
      cumulative_stats.packet_len[i] += ndpi_thread_info[thread_id].workflow->stats.packet_len[i];
    cumulative_stats.max_packet_len += ndpi_thread_info[thread_id].workflow->stats.max_packet_len;
  }
  
  if(cumulative_stats.total_wire_bytes == 0)
    goto free_stats;
  
  if(!quiet_mode) {
    printf("\nnDPI Memory statistics:\n");
    printf("\tnDPI Memory (once):      %-13s\n", formatBytes(ndpi_get_ndpi_detection_module_size(), buf, sizeof(buf)));
    printf("\tFlow Memory (per flow):  %-13s\n", formatBytes(sizeof(struct ndpi_flow_struct), buf, sizeof(buf)));
    printf("\tActual Memory:           %-13s\n", formatBytes(current_ndpi_memory, buf, sizeof(buf)));
    printf("\tPeak Memory:             %-13s\n", formatBytes(max_ndpi_memory, buf, sizeof(buf)));
    printf("\tSetup Time:              %lu msec\n", (unsigned long)(setup_time_usec/1000));
    printf("\tPacket Processing Time:  %lu msec\n", (unsigned long)(processing_time_usec/1000));
    
    if(!json_flag) {
      printf("\nTraffic statistics:\n");
      printf("\tEthernet bytes:        %-13llu (includes ethernet CRC/IFC/trailer)\n",
	     (long long unsigned int)cumulative_stats.total_wire_bytes);
      printf("\tDiscarded bytes:       %-13llu\n",
	     (long long unsigned int)cumulative_stats.total_discarded_bytes);
      printf("\tIP packets:            %-13llu of %llu packets total\n",
	     (long long unsigned int)cumulative_stats.ip_packet_count,
	     (long long unsigned int)cumulative_stats.raw_packet_count);
      /* In order to prevent Floating point exception in case of no traffic*/
      if(cumulative_stats.total_ip_bytes && cumulative_stats.raw_packet_count)
	avg_pkt_size = (unsigned int)(cumulative_stats.total_ip_bytes/cumulative_stats.raw_packet_count);
      printf("\tIP bytes:              %-13llu (avg pkt size %u bytes)\n",
	     (long long unsigned int)cumulative_stats.total_ip_bytes,avg_pkt_size);
      printf("\tUnique flows:          %-13u\n", cumulative_stats.ndpi_flow_count);

      printf("\tTCP Packets:           %-13lu\n", (unsigned long)cumulative_stats.tcp_count);
      printf("\tUDP Packets:           %-13lu\n", (unsigned long)cumulative_stats.udp_count);
      printf("\tVLAN Packets:          %-13lu\n", (unsigned long)cumulative_stats.vlan_count);
      printf("\tMPLS Packets:          %-13lu\n", (unsigned long)cumulative_stats.mpls_count);
      printf("\tPPPoE Packets:         %-13lu\n", (unsigned long)cumulative_stats.pppoe_count);
      printf("\tFragmented Packets:    %-13lu\n", (unsigned long)cumulative_stats.fragmented_count);
      printf("\tMax Packet size:       %-13u\n",   cumulative_stats.max_packet_len);
      printf("\tPacket Len < 64:       %-13lu\n", (unsigned long)cumulative_stats.packet_len[0]);
      printf("\tPacket Len 64-128:     %-13lu\n", (unsigned long)cumulative_stats.packet_len[1]);
      printf("\tPacket Len 128-256:    %-13lu\n", (unsigned long)cumulative_stats.packet_len[2]);
      printf("\tPacket Len 256-1024:   %-13lu\n", (unsigned long)cumulative_stats.packet_len[3]);
      printf("\tPacket Len 1024-1500:  %-13lu\n", (unsigned long)cumulative_stats.packet_len[4]);
      printf("\tPacket Len > 1500:     %-13lu\n", (unsigned long)cumulative_stats.packet_len[5]);

      if(processing_time_usec > 0) {
	char buf[32], buf1[32], when[64];
	float t = (float)(cumulative_stats.ip_packet_count*1000000)/(float)processing_time_usec;
	float b = (float)(cumulative_stats.total_wire_bytes * 8 *1000000)/(float)processing_time_usec;
	float traffic_duration;
	
	if(live_capture) traffic_duration = processing_time_usec;
	else traffic_duration = (pcap_end.tv_sec*1000000 + pcap_end.tv_usec) - (pcap_start.tv_sec*1000000 + pcap_start.tv_usec);
	
	printf("\tnDPI throughput:       %s pps / %s/sec\n", formatPackets(t, buf), formatTraffic(b, 1, buf1));
	t = (float)(cumulative_stats.ip_packet_count*1000000)/(float)traffic_duration;
	b = (float)(cumulative_stats.total_wire_bytes * 8 *1000000)/(float)traffic_duration;

	strftime(when, sizeof(when), "%d/%b/%Y %H:%M:%S", localtime(&pcap_start.tv_sec));
	printf("\tAnalysis begin:        %s\n", when);
	strftime(when, sizeof(when), "%d/%b/%Y %H:%M:%S", localtime(&pcap_end.tv_sec));
	printf("\tAnalysis end:          %s\n", when);
	printf("\tTraffic throughput:    %s pps / %s/sec\n", formatPackets(t, buf), formatTraffic(b, 1, buf1));
	printf("\tTraffic duration:      %.3f sec\n", traffic_duration/1000000);	
      }

      if(enable_protocol_guess)
	printf("\tGuessed flow protos:   %-13u\n", cumulative_stats.guessed_flow_protocols);
    }
  }

  if(json_flag) {
#ifdef HAVE_JSON_C
    if(!strcmp(_jsonFilePath, "-"))
      json_fp = stderr, dont_close_json_fp = 1;
    else if((json_fp = fopen(_jsonFilePath,"w")) == NULL) {
      printf("Error creating .json file %s\n", _jsonFilePath);
      json_flag = 0;
    }

    if(json_flag) {
      jObj_main = json_object_new_object();
      jObj_trafficStats = json_object_new_object();
      jArray_detProto = json_object_new_array();

      json_object_object_add(jObj_trafficStats,"ethernet.bytes",json_object_new_int64(cumulative_stats.total_wire_bytes));
      json_object_object_add(jObj_trafficStats,"discarded.bytes",json_object_new_int64(cumulative_stats.total_discarded_bytes));
      json_object_object_add(jObj_trafficStats,"ip.packets",json_object_new_int64(cumulative_stats.ip_packet_count));
      json_object_object_add(jObj_trafficStats,"total.packets",json_object_new_int64(cumulative_stats.raw_packet_count));
      json_object_object_add(jObj_trafficStats,"ip.bytes",json_object_new_int64(cumulative_stats.total_ip_bytes));
      json_object_object_add(jObj_trafficStats,"avg.pkt.size",json_object_new_int(cumulative_stats.total_ip_bytes/cumulative_stats.raw_packet_count));
      json_object_object_add(jObj_trafficStats,"unique.flows",json_object_new_int(cumulative_stats.ndpi_flow_count));
      json_object_object_add(jObj_trafficStats,"tcp.pkts",json_object_new_int64(cumulative_stats.tcp_count));
      json_object_object_add(jObj_trafficStats,"udp.pkts",json_object_new_int64(cumulative_stats.udp_count));
      json_object_object_add(jObj_trafficStats,"vlan.pkts",json_object_new_int64(cumulative_stats.vlan_count));
      json_object_object_add(jObj_trafficStats,"mpls.pkts",json_object_new_int64(cumulative_stats.mpls_count));
      json_object_object_add(jObj_trafficStats,"pppoe.pkts",json_object_new_int64(cumulative_stats.pppoe_count));
      json_object_object_add(jObj_trafficStats,"fragmented.pkts",json_object_new_int64(cumulative_stats.fragmented_count));
      json_object_object_add(jObj_trafficStats,"max.pkt.size",json_object_new_int(cumulative_stats.max_packet_len));
      json_object_object_add(jObj_trafficStats,"pkt.len_min64",json_object_new_int64(cumulative_stats.packet_len[0]));
      json_object_object_add(jObj_trafficStats,"pkt.len_64_128",json_object_new_int64(cumulative_stats.packet_len[1]));
      json_object_object_add(jObj_trafficStats,"pkt.len_128_256",json_object_new_int64(cumulative_stats.packet_len[2]));
      json_object_object_add(jObj_trafficStats,"pkt.len_256_1024",json_object_new_int64(cumulative_stats.packet_len[3]));
      json_object_object_add(jObj_trafficStats,"pkt.len_1024_1500",json_object_new_int64(cumulative_stats.packet_len[4]));
      json_object_object_add(jObj_trafficStats,"pkt.len_grt1500",json_object_new_int64(cumulative_stats.packet_len[5]));
      json_object_object_add(jObj_trafficStats,"guessed.flow.protos",json_object_new_int(cumulative_stats.guessed_flow_protocols));

      json_object_object_add(jObj_main,"traffic.statistics",jObj_trafficStats);
    }
#endif
  }

  if((!json_flag) && (!quiet_mode)) printf("\n\nDetected protocols:\n");
  for(i = 0; i <= ndpi_get_num_supported_protocols(ndpi_thread_info[0].workflow->ndpi_struct); i++) {
    ndpi_protocol_breed_t breed = ndpi_get_proto_breed(ndpi_thread_info[0].workflow->ndpi_struct, i);

    if(cumulative_stats.protocol_counter[i] > 0) {
      breed_stats[breed] += (long long unsigned int)cumulative_stats.protocol_counter_bytes[i];

      if(results_file)
	fprintf(results_file, "%s\t%llu\t%llu\t%u\n",
		ndpi_get_proto_name(ndpi_thread_info[0].workflow->ndpi_struct, i),
		(long long unsigned int)cumulative_stats.protocol_counter[i],
		(long long unsigned int)cumulative_stats.protocol_counter_bytes[i],
		cumulative_stats.protocol_flows[i]);

      if((!json_flag) && (!quiet_mode)) {
	printf("\t%-20s packets: %-13llu bytes: %-13llu "
	       "flows: %-13u\n",
	       ndpi_get_proto_name(ndpi_thread_info[0].workflow->ndpi_struct, i),
	       (long long unsigned int)cumulative_stats.protocol_counter[i],
	       (long long unsigned int)cumulative_stats.protocol_counter_bytes[i],
	       cumulative_stats.protocol_flows[i]);
      } else {
#ifdef HAVE_JSON_C
	if(json_fp) {
	  jObj = json_object_new_object();

	  json_object_object_add(jObj,"name",json_object_new_string(ndpi_get_proto_name(ndpi_thread_info[0].workflow->ndpi_struct, i)));
	  json_object_object_add(jObj,"breed",json_object_new_string(ndpi_get_proto_breed_name(ndpi_thread_info[0].workflow->ndpi_struct, breed)));
	  json_object_object_add(jObj,"packets",json_object_new_int64(cumulative_stats.protocol_counter[i]));
	  json_object_object_add(jObj,"bytes",json_object_new_int64(cumulative_stats.protocol_counter_bytes[i]));
	  json_object_object_add(jObj,"flows",json_object_new_int(cumulative_stats.protocol_flows[i]));

	  json_object_array_add(jArray_detProto,jObj);
	}
#endif
      }

      total_flow_bytes += cumulative_stats.protocol_counter_bytes[i];
    }
  }

  if((!json_flag) && (!quiet_mode)) {
    printf("\n\nProtocol statistics:\n");

    for(i=0; i < NUM_BREEDS; i++) {
      if(breed_stats[i] > 0) {
	printf("\t%-20s %13llu bytes\n",
	       ndpi_get_proto_breed_name(ndpi_thread_info[0].workflow->ndpi_struct, i),
	       breed_stats[i]);
      }
    }
  }

  // printf("\n\nTotal Flow Traffic: %llu (diff: %llu)\n", total_flow_bytes, cumulative_stats.total_ip_bytes-total_flow_bytes);
 
  if((verbose == 1) || (verbose == 2)) {
    FILE *out = results_file ? results_file : stdout;
    u_int32_t total_flows = 0;

    for(thread_id = 0; thread_id < num_threads; thread_id++)
      total_flows += ndpi_thread_info[thread_id].workflow->num_allocated_flows;

    if((all_flows = (struct flow_info*)malloc(sizeof(struct flow_info)*total_flows)) == NULL) {
      printf("Fatal error: not enough memory\n");
      exit(-1);
    }

    if(!json_flag) fprintf(out, "\n");

    num_flows = 0;
    for(thread_id = 0; thread_id < num_threads; thread_id++) {
      for(i=0; i<NUM_ROOTS; i++)
        ndpi_twalk(ndpi_thread_info[thread_id].workflow->ndpi_flows_root[i], node_print_known_proto_walker, &thread_id);
    } 

    qsort(all_flows, num_flows, sizeof(struct flow_info), cmpFlows);

    for(i=0; i<num_flows; i++)
      printFlow(i+1, all_flows[i].flow, all_flows[i].thread_id);

    for(thread_id = 0; thread_id < num_threads; thread_id++) {
      if(ndpi_thread_info[thread_id].workflow->stats.protocol_counter[0 /* 0 = Unknown */] > 0) {
        if(!json_flag) {
	  FILE *out = results_file ? results_file : stdout;

          fprintf(out, "\n\nUndetected flows:%s\n", undetected_flows_deleted ? " (expired flows are not listed below)" : "");
        }

	if(json_flag)
	  json_flag = 2;
        break;
      }
    }

    num_flows = 0;
    for(thread_id = 0; thread_id < num_threads; thread_id++) {
      if(ndpi_thread_info[thread_id].workflow->stats.protocol_counter[0] > 0) {
        for(i=0; i<NUM_ROOTS; i++)
	  ndpi_twalk(ndpi_thread_info[thread_id].workflow->ndpi_flows_root[i], node_print_unknown_proto_walker, &thread_id);
      }
    }

    qsort(all_flows, num_flows, sizeof(struct flow_info), cmpFlows);

    for(i=0; i<num_flows; i++)
      printFlow(i+1, all_flows[i].flow, all_flows[i].thread_id);

    free(all_flows);
  }

  if(json_flag != 0) {
#ifdef HAVE_JSON_C
    json_object_object_add(jObj_main,"detected.protos",jArray_detProto);
    json_object_object_add(jObj_main,"known.flows",jArray_known_flows);

    if(json_object_array_length(jArray_unknown_flows) != 0)
      json_object_object_add(jObj_main,"unknown.flows",jArray_unknown_flows);

    fprintf(json_fp,"%s\n",json_object_to_json_string(jObj_main));
    if(!dont_close_json_fp) fclose(json_fp);
#endif
  }

  if(stats_flag || verbose == 3) {
    HASH_SORT(srcStats, port_stats_sort);
    HASH_SORT(dstStats, port_stats_sort);
  }

  if(verbose == 3) {
    printf("\n\nSource Ports Stats:\n");
    printPortStats(srcStats);

    printf("\nDestination Ports Stats:\n");
    printPortStats(dstStats);
  }

  if(stats_flag) {
#ifdef HAVE_JSON_C
    json_object *jObj_stats = json_object_new_object();
    char timestamp[64];
    int count;

    strftime(timestamp, sizeof(timestamp), "%FT%TZ", localtime(&pcap_start.tv_sec));
    json_object_object_add(jObj_stats, "time", json_object_new_string(timestamp));

    saveScannerStats(&jObj_stats, &scannerHosts);

    if((count = HASH_COUNT(topReceivers)) == 0){
      HASH_SORT(receivers, receivers_sort);
      saveReceiverStats(&jObj_stats, &receivers, cumulative_stats.ip_packet_count);
    }
    else{
      HASH_SORT(topReceivers, receivers_sort);
      saveReceiverStats(&jObj_stats, &topReceivers, cumulative_stats.ip_packet_count);
    }

    u_int64_t total_src_addr = getTopStats(srcStats);
    u_int64_t total_dst_addr = getTopStats(dstStats);

    saveTopStats(&jObj_stats, &srcStats, DIR_SRC,
                 cumulative_stats.ndpi_flow_count, total_src_addr);

    saveTopStats(&jObj_stats, &dstStats, DIR_DST,
                 cumulative_stats.ndpi_flow_count, total_dst_addr);

    json_object_array_add(jArray_topStats, jObj_stats);
#endif
  }
free_stats:
  if(scannerHosts) {
    deleteScanners(scannerHosts);
    scannerHosts = NULL;
  }

  if(receivers){
    deleteReceivers(receivers);
    receivers = NULL;
  }

  if(topReceivers){
    deleteReceivers(topReceivers);
    topReceivers = NULL;
  }

  if(srcStats) {
    deletePortsStats(srcStats);
    srcStats = NULL;
  }

  if(dstStats) {
    deletePortsStats(dstStats);
    dstStats = NULL;
  }

}

/**
 * @brief Force a pcap_dispatch() or pcap_loop() call to return
 */
static void breakPcapLoop(u_int16_t thread_id) {
#ifdef USE_DPDK
  dpdk_run_capture = 0;
#else
  if(ndpi_thread_info[thread_id].workflow->pcap_handle != NULL) {
    pcap_breakloop(ndpi_thread_info[thread_id].workflow->pcap_handle);
  }
#endif
}

/**
 * @brief Sigproc is executed for each packet in the pcap file
 */
void sigproc(int sig) {
  keepThreadsRunning = 0;

  static int called = 0;
  int thread_id;

  if(called) return; else called = 1;
  shutdown_app = 1;

#ifdef USE_DPDK
  for(thread_id=0; thread_id<num_threads; thread_id++) {
#else
  for(thread_id=0; thread_id<num_pcap_files; thread_id++) {
#endif
  //for(thread_id=0; thread_id<num_threads; thread_id++)
    breakPcapLoop(thread_id);
  }
}



/**
 * @brief Get the next pcap file from a passed playlist
 */
static int getNextPcapFileFromPlaylist(u_int16_t thread_id, char filename[], u_int32_t filename_len) {

  if(playlist_fp[thread_id] == NULL) {
    if((playlist_fp[thread_id] = fopen(_pcap_file[thread_id], "r")) == NULL)
      return -1;
  }

next_line:
  if(fgets(filename, filename_len, playlist_fp[thread_id])) {
    int l = strlen(filename);
    if(filename[0] == '\0' || filename[0] == '#') goto next_line;
    if(filename[l-1] == '\n') filename[l-1] = '\0';
    return 0;
  } else {
    fclose(playlist_fp[thread_id]);
    playlist_fp[thread_id] = NULL;
    return -1;
  }
}


/**
 * @brief Configure the pcap handle
 */
static void configurePcapHandle(pcap_t * pcap_handle) {

  if(bpfFilter != NULL) {
    struct bpf_program fcode;

    if(pcap_compile(pcap_handle, &fcode, bpfFilter, 1, 0xFFFFFF00) < 0) {
      printf("pcap_compile error: '%s'\n", pcap_geterr(pcap_handle));
    } else {
      if(pcap_setfilter(pcap_handle, &fcode) < 0) {
	printf("pcap_setfilter error: '%s'\n", pcap_geterr(pcap_handle));
      } else
	printf("Successfully set BPF filter to '%s'\n", bpfFilter);
    }
  }
}


/**
 * @brief Open a pcap file or a specified device - Always returns a valid pcap_t
 */
static pcap_t * openPcapFileOrDevice(u_int16_t thread_id, const u_char * pcap_file) {
  u_int snaplen = 1536;
  int promisc = 1;
  char pcap_error_buffer[PCAP_ERRBUF_SIZE];
  pcap_t * pcap_handle = NULL;

  /* trying to open a live interface */
#ifdef USE_DPDK
    if (mbufInit == 1) {
      mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS,
							  MBUF_CACHE_SIZE, 0,
							  RTE_MBUF_DEFAULT_BUF_SIZE,
							  rte_socket_id());

    if (mbuf_pool == NULL) {
      rte_exit(EXIT_FAILURE, "Cannot create mbuf pool: are hugepages ok?\n");
    }

    if(dpdk_port_init(dpdk_port_id, mbuf_pool) != 0)
      rte_exit(EXIT_FAILURE, "DPDK: Cannot init port %u: please see README.dpdk\n", dpdk_port_id);

    mbufInit = 0;
    }
    
#else
  if((pcap_handle = pcap_open_live((char*)pcap_file, snaplen,
				   promisc, 500, pcap_error_buffer)) == NULL) {
    capture_for = capture_until = 0;

    live_capture = 0;
    //num_threads = 1; /* Open pcap files in single threads mode */

    /* trying to open a pcap file */
    if((pcap_handle = pcap_open_offline((char*)pcap_file, pcap_error_buffer)) == NULL) {
      char filename[256] = { 0 };

      if(strstr((char*)pcap_file, (char*)".pcap"))
	printf("ERROR: could not open pcap file %s: %s\n", pcap_file, pcap_error_buffer);
      else if((getNextPcapFileFromPlaylist(thread_id, filename, sizeof(filename)) != 0)
	      || ((pcap_handle = pcap_open_offline(filename, pcap_error_buffer)) == NULL)) {
        printf("ERROR: could not open playlist %s: %s\n", filename, pcap_error_buffer);
        exit(-1);
      } else {
        if((!json_flag) && (!quiet_mode))
	  printf("Reading packets from playlist %s...\n", pcap_file);
      }
    } else {
      if((!json_flag) && (!quiet_mode))
	printf("Reading packets from pcap file %s...\n", pcap_file);
    }
  } else {
    live_capture = 1;

    if((!json_flag) && (!quiet_mode)) {
#ifdef USE_DPDK
      printf("Capturing from DPDK (port 0)...\n");
#else
      printf("Capturing live traffic from device %s...\n", pcap_file);
#endif
    }
  }

  configurePcapHandle(pcap_handle);
#endif /* !DPDK */

  if(capture_for > 0) {
    if((!json_flag) && (!quiet_mode))
      printf("Capturing traffic up to %u seconds\n", (unsigned int)capture_for);

#ifndef WIN32
    alarm(capture_for);
    signal(SIGALRM, sigproc);
#endif
  }

  return pcap_handle;
}

/**
 * @brief Check pcap packet
 */
static void ndpi_process_packet(u_char *args,
				struct Node* node) {
  struct ndpi_proto p;
  u_int16_t thread_id = *((u_int16_t*)args);

  const u_char *packet = malloc(sizeof(u_char));
  packet = (const u_char *)node->data;
  struct pcap_pkthdr *header = malloc(sizeof(struct pcap_pkthdr));
  header = node->header;

  u_int8_t protocol_hash = node->protocol;
  u_int16_t vlan_id_hash = node->vlan_id;
  u_int32_t src_ip_hash = node->src_ip;
  u_int32_t dst_ip_hash = node->dst_ip;
  u_int16_t src_port_hash = node->src_port;
  u_int16_t dst_port_hash = node->dst_port;  

  free(node);

  //printf("This is the data: %s\n", packet);

  /* allocate an exact size buffer to check overflows */
  uint8_t *packet_checked = malloc(header->caplen);
  memcpy(packet_checked, packet, header->caplen);

  p = ndpi_workflow_process_packet(ndpi_thread_info[thread_id].workflow, header, packet_checked, &protocol_hash, &vlan_id_hash, &src_ip_hash, &dst_ip_hash, 
           &src_port_hash, &dst_port_hash);

  if((capture_until != 0) && (header->ts.tv_sec >= capture_until)) {
    if(ndpi_thread_info[thread_id].workflow->pcap_handle != NULL)
      pcap_breakloop(ndpi_thread_info[thread_id].workflow->pcap_handle);
    return;
  }
  if(!pcap_start.tv_sec) pcap_start.tv_sec = header->ts.tv_sec, pcap_start.tv_usec = header->ts.tv_usec;
  pcap_end.tv_sec = header->ts.tv_sec, pcap_end.tv_usec = header->ts.tv_usec;

  /* Idle flows cleanup */
  if(live_capture) {
    if(ndpi_thread_info[thread_id].last_idle_scan_time + IDLE_SCAN_PERIOD < ndpi_thread_info[thread_id].workflow->last_time) {
      /* scan for idle flows */
      ndpi_twalk(ndpi_thread_info[thread_id].workflow->ndpi_flows_root[ndpi_thread_info[thread_id].idle_scan_idx],
		 node_idle_scan_walker, &thread_id);

      /* remove idle flows (unfortunately we cannot do this inline) */
      while(ndpi_thread_info[thread_id].num_idle_flows > 0) {
	/* search and delete the idle flow from the "ndpi_flow_root" (see struct reader thread) - here flows are the node of a b-tree */
	ndpi_tdelete(ndpi_thread_info[thread_id].idle_flows[--ndpi_thread_info[thread_id].num_idle_flows],
		     &ndpi_thread_info[thread_id].workflow->ndpi_flows_root[ndpi_thread_info[thread_id].idle_scan_idx],
		     ndpi_workflow_node_cmp);

	/* free the memory associated to idle flow in "idle_flows" - (see struct reader thread)*/
	ndpi_free_flow_info_half(ndpi_thread_info[thread_id].idle_flows[ndpi_thread_info[thread_id].num_idle_flows]);
	ndpi_free(ndpi_thread_info[thread_id].idle_flows[ndpi_thread_info[thread_id].num_idle_flows]);
      }

      if(++ndpi_thread_info[thread_id].idle_scan_idx == ndpi_thread_info[thread_id].workflow->prefs.num_roots) ndpi_thread_info[thread_id].idle_scan_idx = 0;
      ndpi_thread_info[thread_id].last_idle_scan_time = ndpi_thread_info[thread_id].workflow->last_time;
    }
  }

#ifdef DEBUG_TRACE
  if(trace) fprintf(trace, "Found %u bytes packet %u.%u\n", header->caplen, p.app_protocol, p.master_protocol);
#endif

  if(extcap_dumper
     && ((extcap_packet_filter == (u_int16_t)-1)
	 || (p.app_protocol == extcap_packet_filter)
	 || (p.master_protocol == extcap_packet_filter)
       )
    ) {
    struct pcap_pkthdr h;
    uint32_t *crc, delta = sizeof(struct ndpi_packet_trailer) + 4 /* ethernet trailer */;
    struct ndpi_packet_trailer *trailer;

    memcpy(&h, header, sizeof(h));

    if(h.caplen > (sizeof(extcap_buf)-sizeof(struct ndpi_packet_trailer) - 4)) {
      printf("INTERNAL ERROR: caplen=%u\n", h.caplen);
      h.caplen = sizeof(extcap_buf)-sizeof(struct ndpi_packet_trailer) - 4;
    }

    trailer = (struct ndpi_packet_trailer*)&extcap_buf[h.caplen];
    memcpy(extcap_buf, packet, h.caplen);
    memset(trailer, 0, sizeof(struct ndpi_packet_trailer));
    trailer->magic = htonl(0x19680924);
    trailer->master_protocol = htons(p.master_protocol), trailer->app_protocol = htons(p.app_protocol);
    ndpi_protocol2name(ndpi_thread_info[thread_id].workflow->ndpi_struct, p, trailer->name, sizeof(trailer->name));
    crc = (uint32_t*)&extcap_buf[h.caplen+sizeof(struct ndpi_packet_trailer)];
    *crc = ethernet_crc32((const void*)extcap_buf, h.caplen+sizeof(struct ndpi_packet_trailer));
    h.caplen += delta;
    h.len += delta;

#ifdef DEBUG_TRACE
    if(trace) fprintf(trace, "Dumping %u bytes packet\n", h.caplen);
#endif

    pcap_dump((u_char*)extcap_dumper, &h, (const u_char *)extcap_buf);
    pcap_dump_flush(extcap_dumper);
  }

  /* check for buffer changes */
  if(memcmp(packet, packet_checked, header->caplen) != 0)
    printf("INTERNAL ERROR: ingress packet was modified by nDPI: this should not happen [thread_id=%u, packetId=%lu, caplen=%u]\n",
	   thread_id, (unsigned long)ndpi_thread_info[thread_id].workflow->stats.raw_packet_count, header->caplen);
  free(packet_checked);

  if((pcap_end.tv_sec-pcap_start.tv_sec) > pcap_analysis_duration) {
    int i;
    u_int64_t processing_time_usec, setup_time_usec;

    gettimeofday(&end, NULL);
    processing_time_usec = end.tv_sec*1000000 + end.tv_usec - (begin.tv_sec*1000000 + begin.tv_usec);
    setup_time_usec = begin.tv_sec*1000000 + begin.tv_usec - (startup_time.tv_sec*1000000 + startup_time.tv_usec);
    
    printResults(processing_time_usec, setup_time_usec);

    for(i=0; i<ndpi_thread_info[thread_id].workflow->prefs.num_roots; i++) {
      ndpi_tdestroy(ndpi_thread_info[thread_id].workflow->ndpi_flows_root[i], ndpi_flow_info_freer);
      ndpi_thread_info[thread_id].workflow->ndpi_flows_root[i] = NULL;

      memset(&ndpi_thread_info[thread_id].workflow->stats, 0, sizeof(struct ndpi_stats));
    }

    if(!quiet_mode)
      printf("\n-------------------------------------------\n\n");

    memcpy(&begin, &end, sizeof(begin));
    memcpy(&pcap_start, &pcap_end, sizeof(pcap_start));
  }
}

/**
 * @brief get packets to process and send to processing
 */
static void process_allocation(u_int16_t thread_id) {
  pcap_t * handler = ndpi_thread_info[thread_id].workflow->pcap_handle;
  struct pcap_pkthdr *header;
  const u_char *packet;

  while (pcap_next_ex(handler, &header, &packet) >= 0)
    {
      printf("len %s:\n",packet);
    }
}

/**
 * @brief Returns thread for which the packet has been hashed to
 */
static int hashedDistributionValue(u_int8_t* protocol_hash, u_int16_t* vlan_id_hash, u_int32_t* src_ip_hash, 
            u_int32_t* dst_ip_hash, u_int16_t* src_port_hash, u_int16_t* dst_port_hash, const struct pcap_pkthdr *header,
						const u_char *packet, pcap_t * handler, u_int8_t decode_tunnels) {
              
  //printf("entering hash function\n");
  /*
   * Declare pointers to packet headers
   */
 
  /* --- Ethernet header --- */
  const struct ndpi_ethhdr *ethernet;
  /* --- LLC header --- */
  const struct ndpi_llc_header_snap *llc;

  /* --- Cisco HDLC header --- */
  const struct ndpi_chdlc *chdlc;

  /* --- Radio Tap header --- */
  const struct ndpi_radiotap_header *radiotap;
  /* --- Wifi header --- */
  const struct ndpi_wifi_header *wifi;

  /* --- MPLS header --- */
  union mpls {
    uint32_t u32;
    struct ndpi_mpls_header mpls;
  } mpls;

  /** --- IP header --- **/
  struct ndpi_iphdr *iph;
  /** --- IPv6 header --- **/
  struct ndpi_ipv6hdr *iph6;

  /* lengths and offsets */
  u_int16_t eth_offset = 0;
  u_int16_t radio_len;
  u_int16_t fc;
  u_int16_t type = 0;
  int wifi_len = 0;
  int pyld_eth_len = 0;
  int check;
  u_int64_t time;
  u_int16_t ip_offset = 0, ip_len;
  u_int16_t frag_off = 0, vlan_id = 0;
  u_int8_t proto = 0;
  u_int32_t label;
  /* counters */
  u_int8_t vlan_packet = 0;
  
  /*** check Data Link type ***/
  int datalink_type;
  
#ifdef USE_DPDK
  datalink_type = DLT_EN10MB;
#else
  datalink_type = (int)pcap_datalink(handler);
#endif
  
datalink_check:
  switch(datalink_type) {
  case DLT_NULL:
    if(ntohl(*((u_int32_t*)&packet[eth_offset])) == 2)
      type = ETH_P_IP;
    else
      type = ETH_P_IPV6;

    ip_offset = 4 + eth_offset;
    break;

    /* Cisco PPP in HDLC-like framing - 50 */
  case DLT_PPP_SERIAL:
    chdlc = (struct ndpi_chdlc *) &packet[eth_offset];
    ip_offset = sizeof(struct ndpi_chdlc); /* CHDLC_OFF = 4 */
    type = ntohs(chdlc->proto_code);
    break;

    /* Cisco PPP - 9 or 104 */
  case DLT_C_HDLC:
  case DLT_PPP:
    chdlc = (struct ndpi_chdlc *) &packet[eth_offset];
    ip_offset = sizeof(struct ndpi_chdlc); /* CHDLC_OFF = 4 */
    type = ntohs(chdlc->proto_code);
    break;

    /* IEEE 802.3 Ethernet - 1 */
  case DLT_EN10MB:
    ethernet = (struct ndpi_ethhdr *) &packet[eth_offset];
    ip_offset = sizeof(struct ndpi_ethhdr) + eth_offset;
    check = ntohs(ethernet->h_proto);

    if(check <= 1500)
      pyld_eth_len = check;
    else if(check >= 1536)
      type = check;

    if(pyld_eth_len != 0) {
      llc = (struct ndpi_llc_header_snap *)(&packet[ip_offset]);
      /* check for LLC layer with SNAP extension */
      if(llc->dsap == SNAP || llc->ssap == SNAP) {
	type = llc->snap.proto_ID;
	ip_offset += + 8;
      }
      /* No SNAP extension - Spanning Tree pkt must be discarted */
      else if(llc->dsap == BSTP || llc->ssap == BSTP) {
	goto v4_warning;
      }
    }
    break;

    /* Linux Cooked Capture - 113 */
  case DLT_LINUX_SLL:
    type = (packet[eth_offset+14] << 8) + packet[eth_offset+15];
    ip_offset = 16 + eth_offset;
    break;

    /* Radiotap link-layer - 127 */
  case DLT_IEEE802_11_RADIO:
    radiotap = (struct ndpi_radiotap_header *) &packet[eth_offset];
    radio_len = radiotap->len;

    /* Check Bad FCS presence */
    if((radiotap->flags & BAD_FCS) == BAD_FCS) {
      //workflow->stats.total_discarded_bytes +=  header->len;
      //return(nproto);
      printf("Bad FCS present\n");
    }

    /* Calculate 802.11 header length (variable) */
    wifi = (struct ndpi_wifi_header*)( packet + eth_offset + radio_len);
    fc = wifi->fc;

    /* check wifi data presence */
    if(FCF_TYPE(fc) == WIFI_DATA) {
      if((FCF_TO_DS(fc) && FCF_FROM_DS(fc) == 0x0) ||
	 (FCF_TO_DS(fc) == 0x0 && FCF_FROM_DS(fc)))
	wifi_len = 26; /* + 4 byte fcs */
    } else   /* no data frames */
      break;

    /* Check ether_type from LLC */
    llc = (struct ndpi_llc_header_snap*)(packet + eth_offset + wifi_len + radio_len);
    if(llc->dsap == SNAP)
      type = ntohs(llc->snap.proto_ID);

    /* Set IP header offset */
    ip_offset = wifi_len + radio_len + sizeof(struct ndpi_llc_header_snap) + eth_offset;
    break;

  case DLT_RAW:
    ip_offset = eth_offset = 0;
    break;

  default:
    /* printf("Unknown datalink %d\n", datalink_type); */
    printf("Unknown datalink %d\n", datalink_type);
    //return(nproto);
  }

  /* check ether type */
  switch(type) {
  case VLAN:
    vlan_id = ((packet[ip_offset] << 8) + packet[ip_offset+1]) & 0xFFF;
    type = (packet[ip_offset+2] << 8) + packet[ip_offset+3];
    ip_offset += 4;
    vlan_packet = 1;
    // double tagging for 802.1Q
    if(type == 0x8100) {
      vlan_id = ((packet[ip_offset] << 8) + packet[ip_offset+1]) & 0xFFF;
      type = (packet[ip_offset+2] << 8) + packet[ip_offset+3];
      ip_offset += 4;
    }
    break;
  case MPLS_UNI:
  case MPLS_MULTI:
    mpls.u32 = *((uint32_t *) &packet[ip_offset]);
    mpls.u32 = ntohl(mpls.u32);
    //workflow->stats.mpls_count++;
    type = ETH_P_IP, ip_offset += 4;

    while(!mpls.mpls.s) {
      mpls.u32 = *((uint32_t *) &packet[ip_offset]);
      mpls.u32 = ntohl(mpls.u32);
      ip_offset += 4;
    }
    break;
  case PPPoE:
    //workflow->stats.pppoe_count++;
    type = ETH_P_IP;
    ip_offset += 8;
    break;
  default:
    break;
  }

  //workflow->stats.vlan_count += vlan_packet;

iph_check:
  /* Check and set IP header size and total packet length */
  iph = (struct ndpi_iphdr *) &packet[ip_offset];

  /* just work on Ethernet packets that contain IP */
  if(type == ETH_P_IP && header->caplen >= ip_offset) {
    frag_off = ntohs(iph->frag_off);

    proto = iph->protocol;
    if(header->caplen < header->len) {
      static u_int8_t cap_warning_used = 0;

      if(cap_warning_used == 0) {
	// if(!workflow->prefs.quiet_mode)
	//   NDPI_LOG(0, workflow->ndpi_struct, NDPI_LOG_DEBUG, "\n\nWARNING: packet capture size is smaller than packet size, DETECTION MIGHT NOT WORK CORRECTLY\n\n");
	cap_warning_used = 1;
      }
    }
  }
  
  if(iph->version == IPVERSION) {
    ip_len = ((u_int16_t)iph->ihl * 4);
    iph6 = NULL;

    if(iph->protocol == IPPROTO_IPV6) {
      ip_offset += ip_len;
      goto iph_check;
    }

    if((frag_off & 0x1FFF) != 0) {
      static u_int8_t ipv4_frags_warning_used = 0;
      //workflow->stats.fragmented_count++;

      if(ipv4_frags_warning_used == 0) {
	// if(!workflow->prefs.quiet_mode)
	//   NDPI_LOG(0, workflow->ndpi_struct, NDPI_LOG_DEBUG, "\n\nWARNING: IPv4 fragments are not handled by this demo (nDPI supports them)\n");
	ipv4_frags_warning_used = 1;
      }

      //workflow->stats.total_discarded_bytes +=  header->len;
      printf("error in hash function\n");
      //return(nproto);
    }
  } else if(iph->version == 6) {
    iph6 = (struct ndpi_ipv6hdr *)&packet[ip_offset];
    proto = iph6->ip6_hdr.ip6_un1_nxt;
    ip_len = sizeof(struct ndpi_ipv6hdr);

    if(proto == IPPROTO_DSTOPTS /* IPv6 destination option */) {

      u_int8_t *options = (u_int8_t*)&packet[ip_offset+ip_len];
      proto = options[0];
      ip_len += 8 * (options[1] + 1);
    }
    iph = NULL;

  } else {
    static u_int8_t ipv4_warning_used = 0;

  v4_warning:
    if(ipv4_warning_used == 0) {
      // if(!workflow->prefs.quiet_mode)
      //   NDPI_LOG(0, workflow->ndpi_struct, NDPI_LOG_DEBUG, "\n\nWARNING: only IPv4/IPv6 packets are supported in this demo (nDPI supports both IPv4 and IPv6), all other packets will be discarded\n\n");
      ipv4_warning_used = 1;
    }
    printf("error in hash function\n");
    //workflow->stats.total_discarded_bytes +=  header->len;
    //return(nproto);
  }
  
  if(decode_tunnels && (proto == IPPROTO_UDP)) {
    struct ndpi_udphdr *udp = (struct ndpi_udphdr *)&packet[ip_offset+ip_len];
    u_int16_t sport = ntohs(udp->source), dport = ntohs(udp->dest);

    if((sport == GTP_U_V1_PORT) || (dport == GTP_U_V1_PORT)) {
      /* Check if it's GTPv1 */
      u_int offset = ip_offset+ip_len+sizeof(struct ndpi_udphdr);
      u_int8_t flags = packet[offset];
      u_int8_t message_type = packet[offset+1];

      if((((flags & 0xE0) >> 5) == 1 /* GTPv1 */) &&
	 (message_type == 0xFF /* T-PDU */)) {

	ip_offset = ip_offset+ip_len+sizeof(struct ndpi_udphdr)+8; /* GTPv1 header len */
	if(flags & 0x04) ip_offset += 1; /* next_ext_header is present */
	if(flags & 0x02) ip_offset += 4; /* sequence_number is present (it also includes next_ext_header and pdu_number) */
	if(flags & 0x01) ip_offset += 1; /* pdu_number is present */

	iph = (struct ndpi_iphdr *) &packet[ip_offset];

	if(iph->version != IPVERSION) {
	  // printf("WARNING: not good (packet_id=%u)!\n", (unsigned int)workflow->stats.raw_packet_count);
	  goto v4_warning;
	}
      }
    } else if((sport == TZSP_PORT) || (dport == TZSP_PORT)) {
      /* https://en.wikipedia.org/wiki/TZSP */
      u_int offset = ip_offset+ip_len+sizeof(struct ndpi_udphdr);
      u_int8_t version = packet[offset];
      u_int8_t type    = packet[offset+1];
      u_int16_t encapsulates = ntohs(*((u_int16_t*)&packet[offset+2]));

      if((version == 1) && (type == 0) && (encapsulates == 1)) {
	u_int8_t stop = 0;

	offset += 4;

	while((!stop) && (offset < header->caplen)) {
	  u_int8_t tag_type = packet[offset];
	  u_int8_t tag_len;

	  switch(tag_type) {
	  case 0: /* PADDING Tag */
	    tag_len = 1;
	    break;
	  case 1: /* END Tag */
	    tag_len = 1, stop = 1;
	    break;
	  default:
	    tag_len = packet[offset+1];
	    break;
	  }

	  offset += tag_len;

	  if(offset >= header->caplen)
      printf("error in hash function, invalid packet\n");
	    //return(nproto); /* Invalid packet */
	  else {
	    eth_offset = offset;
	    goto datalink_check;
	  }
	}
      }
    }
  }
  
  u_int8_t protocol = iph->protocol;
  u_int16_t src_port, dst_port, l4_packet_len, sport, dport;
  u_int32_t hashval, src_ip = iph->saddr, dst_ip = iph->daddr, l4_offset;

  struct ndpi_tcphdr *tcphStruct = NULL;
  struct ndpi_udphdr *udphStruct = NULL;
  struct ndpi_tcphdr **tcph;
  struct ndpi_udphdr **udph;
  tcph = &tcphStruct;
  udph = &udphStruct;

  const u_int8_t *l3, *l4;
  
  /*
    Note: to keep things simple (ndpiReader is just a demo app)
    we handle IPv6 a-la-IPv4.
  */
 
  const u_int8_t version = IPVERSION;
  u_int16_t ipsize = header->caplen - ip_offset;
  if(version == IPVERSION) {
    if(ipsize < 20)
    printf("IP size is less than 20, error");
      // return NULL;

    if((iph->ihl * 4) > ipsize || ipsize < ntohs(iph->tot_len)
       /* || (iph->frag_off & htons(0x1FFF)) != 0 */)
      printf("IP size is less than 20, error");
      //return NULL;

    l4_offset = iph->ihl * 4;
    l3 = (const u_int8_t*)iph;
  
  } else {
    l4_offset = sizeof(struct ndpi_ipv6hdr);
    l3 = (const u_int8_t*)iph6;
  }
  
  l4_packet_len = ntohs(iph->tot_len) - (iph->ihl * 4);
  
  l4 = ((const u_int8_t *) l3 + l4_offset);

  // SEG FAULTS HERE.... :(
  if(iph->protocol == IPPROTO_TCP && l4_packet_len >= 20) {
    // tcp
    *tcph = (struct ndpi_tcphdr *)l4;
    sport = ntohs((*tcph)->source);
    dport = ntohs((*tcph)->dest);
  } else if(iph->protocol == IPPROTO_UDP && l4_packet_len >= 8) {
    // udp
    *udph = (struct ndpi_udphdr *)l4;
    sport = ntohs((*udph)->source), dport = ntohs((*udph)->dest);
  } else {
    // non tcp/udp protocols
    sport = dport = 0;
  }
  src_port = htons(sport);
  dst_port = htons(dport);
  hashval = protocol + vlan_id + src_ip + dst_ip + src_port + dst_port; 

   *protocol_hash = protocol;
   *vlan_id_hash = vlan_id;
   *src_ip_hash = src_ip;
   *dst_ip_hash = dst_ip;
   *src_port_hash = src_port;
   *dst_port_hash = dst_port;

  
  u_int32_t idx = hashval % num_threads;
  return idx;
}

/**
 * @brief Call pcap_loop() to process packets from a live capture or savefile
 */
static void runPcapLoop(u_int16_t thread_id) {
  if((!shutdown_app) && (ndpi_thread_info[thread_id].workflow->pcap_handle != NULL)) {
    pcap_t * handler = ndpi_thread_info[thread_id].workflow->pcap_handle;
    struct pcap_pkthdr *header;
    const u_char *packet;
     int count = 0;
    while (pcap_next_ex(handler, &header, &packet) >= 0) {
      u_char *element;
      element = (u_char*)packet;

      if (count == 0) {
        sentPacketTemp = element;
      } else {
        count = 1;
      }

      u_int8_t protocol_hash;
      u_int16_t vlan_id_hash;
      u_int32_t src_ip_hash;
      u_int32_t dst_ip_hash;
      u_int16_t src_port_hash;
      u_int16_t dst_port_hash;

      int hashValue = hashedDistributionValue(&protocol_hash, &vlan_id_hash, &src_ip_hash, &dst_ip_hash, 
           &src_port_hash, &dst_port_hash, header, packet, handler,
           ndpi_thread_info[thread_id].workflow->prefs.decode_tunnels);

      pthread_mutex_t *lock = locks[hashValue];
      //printf("lock: %p\n", lock);
      pthread_mutex_lock(lock);
      struct Node *rear = threadQueueRears[hashValue]; 
      struct Node *front = threadQueueFronts[hashValue];

      enqueue(element, &header, &front, &rear, &protocol_hash, &vlan_id_hash, &src_ip_hash, &dst_ip_hash, 
           &src_port_hash, &dst_port_hash);
      threadQueueRears[hashValue] = rear;
      threadQueueFronts[hashValue] = front;
      pthread_mutex_unlock(lock);

      // pthread_mutex_t *lock = locks[count];
      // pthread_mutex_lock(lock);
      // struct Node *rear = threadQueueRears[count]; 
      // struct Node *front = threadQueueFronts[count];
      // enqueue(&element, &header, &front, &rear, &protocol_hash, &vlan_id_hash, &src_ip_hash, &dst_ip_hash, 
      //      &src_port_hash, &dst_port_hash);
      // threadQueueRears[count] = rear;
      // threadQueueFronts[count] = front;
      // pthread_mutex_unlock(lock);

      // count++;
      // if (count == num_threads) {
      //   count = 0;
      // }
    }

    int queuesNotEmpty = 1;
    int queuesEmpty[num_threads];
    for (int i = 0; i < num_threads; i++) {
      queuesEmpty[i] = 0;
    }

    while (queuesNotEmpty) {
      for (int i = 0; i < num_threads; i++) {
        struct Node *rear = threadQueueRears[i]; 
        struct Node *front = threadQueueFronts[i];
        if (isEmpty(&front, &rear)) {
          queuesEmpty[i] = 1;
        }
      }
      queuesNotEmpty = 0; 
      for (int i = 0; i < num_threads; i++) {
        if (queuesEmpty[i] == 0) {
          queuesNotEmpty = 1; 
        }
      }
    }
  }
}

/**
 * @brief Process a running thread
 */
void * processing_thread(void *_thread_id) {
  long thread_id = (long) _thread_id;
  char pcap_error_buffer[PCAP_ERRBUF_SIZE];

#if defined(linux) && defined(HAVE_PTHREAD_SETAFFINITY_NP)
  if(core_affinity[thread_id] >= 0) {
    cpu_set_t cpuset;
    
    CPU_ZERO(&cpuset);
    CPU_SET(core_affinity[thread_id], &cpuset);

    if(pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0)
      fprintf(stderr, "Error while binding thread %ld to core %d\n", thread_id, core_affinity[thread_id]);
    else {
      if((!json_flag) && (!quiet_mode)) printf("Running thread %ld on core %d...\n", thread_id, core_affinity[thread_id]);
    }
  } else
#endif

#ifdef USE_DPDK
  gettimeofday(&startSlice, NULL);
  while(dpdk_run_capture) {
    struct rte_mbuf *bufs[BURST_SIZE];
    u_int16_t num = rte_eth_rx_burst(dpdk_port_id, 0, bufs, BURST_SIZE);
    u_int i;
    
    if(num == 0) {
      usleep(1);
      continue;
    }
    
    for(i = 0; i < PREFETCH_OFFSET && i < num; i++)
      rte_prefetch0(rte_pktmbuf_mtod(bufs[i], void *));
    
    for(i = 0; i < num; i++) {
      
      char *data = rte_pktmbuf_mtod(bufs[i], char *);
      u_char* element = (u_char*)data; 
      
      int len = rte_pktmbuf_pkt_len(bufs[i]);
      struct pcap_pkthdr h;

      h.len = h.caplen = len;
      
      gettimeofday(&h.ts, NULL);
      struct pcap_pkthdr* header = &h;
      
      pcap_t * handler = ndpi_thread_info[thread_id].workflow->pcap_handle;

      u_int8_t protocol_hash;
      u_int16_t vlan_id_hash;
      u_int32_t src_ip_hash;
      u_int32_t dst_ip_hash;
      u_int16_t src_port_hash;
      u_int16_t dst_port_hash;

      int hashValue = hashedDistributionValue(&protocol_hash, &vlan_id_hash, &src_ip_hash, &dst_ip_hash, 
           &src_port_hash, &dst_port_hash, header, element, handler,
           ndpi_thread_info[thread_id].workflow->prefs.decode_tunnels);

      //int hashValue = hashedDistributionValue(header, element, handler, ndpi_thread_info[thread_id].workflow->prefs.decode_tunnels);
      pthread_mutex_t *lock = locks[hashValue];
      pthread_mutex_lock(lock);
      struct Node *rear = threadQueueRears[hashValue]; 
      struct Node *front = threadQueueFronts[hashValue];

      // enqueue(&element, &header, &front, &rear, &protocol_hash, &vlan_id_hash, &src_ip_hash, &dst_ip_hash, 
      //      &src_port_hash, &dst_port_hash);
      //enqueue(&element, &header, &front, &rear);
      threadQueueRears[hashValue] = rear;
      threadQueueFronts[hashValue] = front;
      pthread_mutex_unlock(lock);

      rte_pktmbuf_free(bufs[i]);
    }

    gettimeofday(&endSlice, NULL);
    if ((endSlice.tv_sec - startSlice.tv_sec) > pauseDur) {
      startSlice.tv_sec = endSlice.tv_sec;
      
      
      u_int64_t processing_time_usec, setup_time_usec;
      processing_time_usec = endSlice.tv_sec*1000000 + endSlice.tv_usec - (startSlice.tv_sec*1000000 + startSlice.tv_usec);
      setup_time_usec = startSlice.tv_sec*1000000 + startSlice.tv_usec - (startup_time.tv_sec*1000000 + startup_time.tv_usec);

      int queuesNotEmpty = 1;
      int queuesEmpty[num_threads];
      for (int i = 0; i < num_threads; i++) {
        queuesEmpty[i] = 0;
      }

      while (queuesNotEmpty) {
        for (int i = 0; i < num_threads; i++) {
          struct Node *rear = threadQueueRears[i]; 
          struct Node *front = threadQueueFronts[i];
          if (isEmpty(&front, &rear)) {
            queuesEmpty[i] = 1;
          }
        }
        queuesNotEmpty = 0; 
        for (int i = 0; i < num_threads; i++) {
          if (queuesEmpty[i] == 0) {
            queuesNotEmpty = 1; 
          }
        }
      }
      
      printResults(processing_time_usec, setup_time_usec);

      ndpi_workflow_reset(ndpi_thread_info[thread_id].workflow);
    }
  }
  
#else
pcap_loop:
  runPcapLoop(thread_id);
  if(playlist_fp[thread_id] != NULL) { /* playlist: read next file */
    char filename[256];

    if(getNextPcapFileFromPlaylist(thread_id, filename, sizeof(filename)) == 0 &&
       (ndpi_thread_info[thread_id].workflow->pcap_handle = pcap_open_offline(filename, pcap_error_buffer)) != NULL) {
      configurePcapHandle(ndpi_thread_info[thread_id].workflow->pcap_handle);
      goto pcap_loop;
    }
  }
#endif

  return NULL;
}


/**
 * @brief Begin, process, end detection process
 */
void test_lib() {
  struct timeval end;
  u_int64_t processing_time_usec, setup_time_usec;
  long thread_id;

#ifdef HAVE_JSON_C
  json_init();
  if(stats_flag)  json_open_stats_file();
#endif

#ifdef DEBUG_TRACE
  if(trace) fprintf(trace, "Num threads: %d\n", num_threads);
#endif

#ifdef USE_DPDK
  pcap_t *cap;
  cap = openPcapFileOrDevice(thread_id, (const u_char*)_pcap_file[thread_id]);
  for(thread_id = 0; thread_id < num_threads; thread_id++) {
  
  #ifdef DEBUG_TRACE
      if(trace) fprintf(trace, "Opening %s\n", (const u_char*)_pcap_file[thread_id]);
  #endif
     setupDetection(thread_id, cap);
  }
#endif

  gettimeofday(&begin, NULL);
  
  start_threads();

  gettimeofday(&end, NULL);
  processing_time_usec = end.tv_sec*1000000 + end.tv_usec - (begin.tv_sec*1000000 + begin.tv_usec);
  setup_time_usec = begin.tv_sec*1000000 + begin.tv_usec - (startup_time.tv_sec*1000000 + startup_time.tv_usec);

  /* Printing cumulative results */
  printResults(processing_time_usec, setup_time_usec);

  if(stats_flag) {
#ifdef HAVE_JSON_C
    json_close_stats_file();
#endif
  }

#ifdef USE_DPDK
  for(thread_id = 0; thread_id < num_threads; thread_id++) {
#else 
  for(thread_id = 0; thread_id < num_threads; thread_id++) {
#endif
    if(ndpi_thread_info[thread_id].workflow->pcap_handle != NULL)
      terminateDetection(thread_id);
  }
}

void automataUnitTest() {
  void *automa;

  assert((automa = ndpi_init_automa()));
  assert(ndpi_add_string_to_automa(automa, "hello") == 0);
  assert(ndpi_add_string_to_automa(automa, "world") == 0);
  ndpi_finalize_automa(automa);
  assert(ndpi_match_string(automa, "This is the wonderful world of nDPI") == 0);

  ndpi_free_automa(automa);
}

/* *********************************************** */
/**
 * @brief Produce bpf filter to filter ports and hosts
 * in order to remove a peak in terms of number of packets
 * sent by source hosts.
 */
#ifdef HAVE_JSON_C
void bpf_filter_pkt_peak_filter(json_object **jObj_bpfFilter,
                                int port_array[], int p_size,
                                const char *src_host_array[16],
                                int sh_size,
                                const char *dst_host_array[16],
                                int dh_size) {
  char filter[2048];
  int produced = 0;
  int i = 0;

  if(port_array[0] != INIT_VAL) {
    int l;

    strcpy(filter, "not (src port ");

    while(i < p_size && port_array[i] != INIT_VAL) {
      l = strlen(filter);

      if(i+1 == p_size || port_array[i+1] == INIT_VAL)
        snprintf(&filter[l], sizeof(filter)-l, "%d", port_array[i]);
      else
        snprintf(&filter[l], sizeof(filter)-l, "%d or ", port_array[i]);
      i++;
    }

    l = strlen(filter);
    snprintf(&filter[l], sizeof(filter)-l, "%s", ")");
    produced = 1;
  }


  if(src_host_array[0] != NULL) {
    int l;

    if(port_array[0] != INIT_VAL)
      strncat(filter, " and not (src ", sizeof(" and not (src "));
    else
      strcpy(filter, "not (src ");


    i=0;
    while(i < sh_size && src_host_array[i] != NULL) {
      l = strlen(filter);

      if(i+1 == sh_size || src_host_array[i+1] == NULL)
	snprintf(&filter[l], sizeof(filter)-l, "%s", src_host_array[i]);
      else
	snprintf(&filter[l], sizeof(filter)-l, "%s or ", src_host_array[i]);

      i++;
    }

    l = strlen(filter);
    snprintf(&filter[l], sizeof(filter)-l, "%s", ")");
    produced = 1;
  }


  if(dst_host_array[0] != NULL) {
    int l;

    if(port_array[0] != INIT_VAL || src_host_array[0] != NULL)
      strncat(filter, " and not (dst ", sizeof(" and not (dst "));
    else
      strcpy(filter, "not (dst ");

    i=0;

    while(i < dh_size && dst_host_array[i] != NULL) {
      l = strlen(filter);

      if(i+1 == dh_size || dst_host_array[i+1] == NULL)
	snprintf(&filter[l], sizeof(filter)-l, "%s", dst_host_array[i]);
      else
	snprintf(&filter[l], sizeof(filter)-l, "%s or ", dst_host_array[i]);

      i++;
    }

    l = strlen(filter);
    snprintf(&filter[l], sizeof(filter)-l, "%s", ")");
    produced = 1;
  }



  if(produced)
    json_object_object_add(*jObj_bpfFilter, "pkt.peak.filter", json_object_new_string(filter));
  else
    json_object_object_add(*jObj_bpfFilter, "pkt.peak.filter", json_object_new_string(""));
}
#endif

/* *********************************************** */
/**
 * @brief Produce bpf filter to filter ports and hosts
 * in order to remove a peak in terms of number of source
 * addresses.
 */
#ifdef HAVE_JSON_C
void bpf_filter_host_peak_filter(json_object **jObj_bpfFilter,
                                 const char *host_array[16],
                                 int h_size) {
  char filter[2048];
  int produced = 0;
  int i = 0;


  if(host_array[0] != NULL) {
    int l;

    strcpy(filter, "not (dst ");

    while(i < h_size && host_array[i] != NULL) {
      l = strlen(filter);

      if(i+1 == h_size || host_array[i+1] == NULL)
	snprintf(&filter[l], sizeof(filter)-l, "%s", host_array[i]);
      else
	snprintf(&filter[l], sizeof(filter)-l, "%s or ", host_array[i]);

      i++;
    }

    l = strlen(filter);
    snprintf(&filter[l], sizeof(filter)-l, "%s", ")");
    produced = 1;
  }

  if(produced)
    json_object_object_add(*jObj_bpfFilter, "host.peak.filter", json_object_new_string(filter));
  else
    json_object_object_add(*jObj_bpfFilter, "host.peak.filter", json_object_new_string(""));
}
#endif

/* *********************************************** */
/**
 * @brief Initialize port array
 */

void bpf_filter_port_array_init(int array[], int size) {
  int i;
  for(i=0; i<size; i++)
    array[i] = INIT_VAL;
}

/* *********************************************** */
/**
 * @brief Initialize host array
 */

void bpf_filter_host_array_init(const char *array[48], int size) {
  int i;
  for(i=0; i<size; i++)
    array[i] = NULL;
}

/* *********************************************** */

/**
 * @brief Add host to host filter array
 */

void bpf_filter_host_array_add(const char *filter_array[48], int size, const char *host) {
  int i;
  int r;
  for(i=0; i<size; i++) {
    if((filter_array[i] != NULL) && (r = strcmp(filter_array[i], host)) == 0)
      return;
    if(filter_array[i] == NULL) {
      filter_array[i] = host;
      return;
    }
  }
  fprintf(stderr,"bpf_filter_host_array_add: max array size is reached!\n");
  exit(-1);
}


/* *********************************************** */

/**
 * @brief Add port to port filter array
 */

void bpf_filter_port_array_add(int filter_array[], int size, int port) {
  int i;
  for(i=0; i<size; i++) {
    if(filter_array[i] == port)
      return;
    if(filter_array[i] == INIT_VAL) {
      filter_array[i] = port;
      return;
    }
  }
  fprintf(stderr,"bpf_filter_port_array_add: max array size is reached!\n");
  exit(-1);
}


/* *********************************************** */
#ifdef HAVE_JSON_C
/*
 * @brief returns average value for a given field
 */
float getAverage(struct json_object *jObj_stat, char *field){
  json_object *field_stat;
  json_bool res;
  float average;
  float sum = 0;
  int r;
  int j;

  if((r = strcmp(field, "top.scanner.stats")) == 0) {
    for(j=0; j<json_object_array_length(jObj_stat); j++) {
      field_stat = json_object_array_get_idx(jObj_stat, j);
      json_object *jObj_tot_flows_number;

      if((res = json_object_object_get_ex(field_stat, "total.flows.number", &jObj_tot_flows_number)) == 0) {
        fprintf(stderr, "ERROR: can't get \"total.flows.number\", use -x flag only with .json files generated by ndpiReader -b flag.\n");
        exit(-1);
      }
      u_int32_t tot_flows_number = json_object_get_int(jObj_tot_flows_number);

      sum += tot_flows_number;
    }
  } else if((r = strcmp(field, "top.src.pkts.stats")) == 0) {
    for(j=0; j<json_object_array_length(jObj_stat); j++) {
      field_stat = json_object_array_get_idx(jObj_stat, j);
      json_object *jObj_packets_number;

      if((res = json_object_object_get_ex(field_stat, "packets.number", &jObj_packets_number)) == 0) {
        fprintf(stderr, "ERROR: can't get \"packets.number\", use -x flag only with .json files generated by ndpiReader -b flag.\n");
        exit(-1);
      }
      u_int32_t packets_number = json_object_get_int(jObj_packets_number);

      sum += packets_number;
    }
  }

  if(j == 0) return 0.0;

  return sum/j;
}
#endif
/* *********************************************** */
#ifdef HAVE_JSON_C
/*
 * @brief returns standard deviation for a given
 * field and it's average value.
 */
float getStdDeviation(struct json_object *jObj_stat, float average, char *field){
  json_object *field_stat;
  json_bool res;
  float sum = 0;
  int j;
  int r;

  if((r = strcmp(field, "top.scanner.stats")) == 0){
    for(j=0; j<json_object_array_length(jObj_stat); j++) {
      field_stat = json_object_array_get_idx(jObj_stat, j);
      json_object *jObj_tot_flows_number;

      if((res = json_object_object_get_ex(field_stat, "total.flows.number", &jObj_tot_flows_number)) == 0) {
        fprintf(stderr, "ERROR: can't get \"total.flows.number\", use -x flag only with .json files generated by ndpiReader -b flag.\n");
        exit(-1);
      }
      u_int32_t tot_flows_number = json_object_get_int(jObj_tot_flows_number);

      sum += pow((tot_flows_number - average), 2);
    }
  }

  return sqrt(sum/(float)j);
}

#endif

/* *********************************************** */

#ifdef HAVE_JSON_C
void getSourcePorts(struct json_object *jObj_stat, int srcPortArray[], int size, float threshold) {
  int j;

  for(j=0; j<json_object_array_length(jObj_stat); j++) {
    json_object *src_pkts_stat = json_object_array_get_idx(jObj_stat, j);
    json_object *jObj_packets_number;
    json_object *jObj_flows_percent;
    json_object *jObj_flows_packets;
    json_object *jObj_port;
    json_bool res;

    if((res = json_object_object_get_ex(src_pkts_stat, "packets.number", &jObj_packets_number)) == 0) {
      fprintf(stderr, "ERROR: can't get \"packets.number\", use -x flag only with .json files generated by ndpiReader -b flag.\n");
      exit(-1);
    }
    u_int32_t packets_number = json_object_get_int(jObj_packets_number);

    if((res = json_object_object_get_ex(src_pkts_stat, "flows.percent", &jObj_flows_percent)) == 0) {
      fprintf(stderr, "ERROR: can't get \"flows.percent\", use -x flag only with .json files generated by ndpiReader -b flag.\n");
      exit(-1);
    }
    double flows_percent = json_object_get_double(jObj_flows_percent);


    if((res = json_object_object_get_ex(src_pkts_stat, "flows/packets", &jObj_flows_packets)) == 0) {
      fprintf(stderr, "ERROR: can't get \"flows/packets\", use -x flag only with .json files generated by ndpiReader -b flag.\n");
      exit(-1);
    }
    double flows_packets = json_object_get_double(jObj_flows_packets);


    if((flows_packets > FLOWS_PACKETS_THRESHOLD)
       && (flows_percent >= FLOWS_PERCENT_THRESHOLD)
       && packets_number >= threshold) {
      if((res = json_object_object_get_ex(src_pkts_stat, "port", &jObj_port)) == 0) {
	fprintf(stderr, "ERROR: can't get \"port\", use -x flag only with .json files generated by ndpiReader -b flag.\n");
	exit(-1);
      }
      int port = json_object_get_int(jObj_port);

      bpf_filter_port_array_add(srcPortArray, size, port);
    }
  }
}
#endif

/* *********************************************** */

#ifdef HAVE_JSON_C
void getReceiverHosts(struct json_object *jObj_stat, const char *dstHostArray[16], int size) {
  int j;

  for(j=0; j<json_object_array_length(jObj_stat); j++) {
    json_object *scanner_stat = json_object_array_get_idx(jObj_stat, j);
    json_object *jObj_host_address;
    json_object *jObj_pkts_percent;
    json_bool res;

    if((res = json_object_object_get_ex(scanner_stat, "packets.percent", &jObj_pkts_percent)) == 0) {
      fprintf(stderr, "ERROR: can't get \"packets.percent\", use -x flag only with .json files generated by ndpiReader -b flag.\n");
      exit(-1);
    }
    double pkts_percent = json_object_get_double(jObj_pkts_percent);


    if(pkts_percent > PKTS_PERCENT_THRESHOLD) {
      if((res = json_object_object_get_ex(scanner_stat, "ip.address", &jObj_host_address)) == 0) {
	fprintf(stderr, "ERROR: can't get \"ip.address, use -x flag only with .json files generated by ndpiReader -b flag.\n");
	exit(-1);
      }
      const char *host_address = json_object_get_string(jObj_host_address);

      bpf_filter_host_array_add(dstHostArray, size, host_address);
    }
  }
}
#endif

/* *********************************************** */

#ifdef HAVE_JSON_C
void getScannerHosts(struct json_object *jObj_stat, int duration,
                     const char *srcHostArray[48], int size,
                     float threshold) {
  int j;

  for(j=0; j<json_object_array_length(jObj_stat); j++) {
    json_object *scanner_stat = json_object_array_get_idx(jObj_stat, j);
    json_object *jObj_host_address;
    json_object *jObj_tot_flows_number;
    json_bool res;


    if((res = json_object_object_get_ex(scanner_stat, "total.flows.number", &jObj_tot_flows_number)) == 0) {
      fprintf(stderr, "ERROR: can't get \"total.flows.number\", use -x flag only with .json files generated by ndpiReader -b flag.\n");
      exit(-1);
    }
    u_int32_t tot_flows_number = json_object_get_int(jObj_tot_flows_number);


    if(((tot_flows_number/(float)duration) > FLOWS_THRESHOLD) && tot_flows_number > threshold) {
      if((res = json_object_object_get_ex(scanner_stat, "ip.address", &jObj_host_address)) == 0) {
	fprintf(stderr, "ERROR: can't get \"ip.address\", use -x flag only with .json files generated by ndpiReader -b flag.\n");
	exit(-1);
      }
      const char *host_address = json_object_get_string(jObj_host_address);

      bpf_filter_host_array_add(srcHostArray, size, host_address);

    }
  }
}
#endif

/* *********************************************** */

#ifdef HAVE_JSON_C
void getDestinationHosts(struct json_object *jObj_stat, int duration,
			 const char *dstHostArray[16], int size) {
  int j;

  for(j=0; j<json_object_array_length(jObj_stat); j++) {
    json_object *scanner_stat = json_object_array_get_idx(jObj_stat, j);
    json_object *jObj_host_address;
    json_object *jObj_flows_percent;
    json_bool res;


    if((res = json_object_object_get_ex(scanner_stat, "flows.percent", &jObj_flows_percent)) == 0) {
      fprintf(stderr, "ERROR: can't get \"flows.percent\", use -x flag only with .json files generated by ndpiReader -b flag.\n");
      exit(-1);
    }
    double flows_percent = json_object_get_double(jObj_flows_percent);


    if(flows_percent > FLOWS_PERCENT_THRESHOLD_2) {
      if((res = json_object_object_get_ex(scanner_stat, "aggressive.host", &jObj_host_address)) == 0) {
	fprintf(stderr, "ERROR: can't get \"aggressive.host\", use -x flag only with .json files generated by ndpiReader -b flag.\n");
	exit(-1);
      }
      const char *host_address = json_object_get_string(jObj_host_address);

      bpf_filter_host_array_add(dstHostArray, size, host_address);

    }
  }
}
#endif

/* *********************************************** */

#ifdef HAVE_JSON_C
static void produceBpfFilter(char *filePath) {
  json_object *jObj; /* entire json object from file */
  json_object *jObj_duration;
  json_object *jObj_statistics; /* json array */
  json_bool res;
  int filterSrcPorts[PORT_ARRAY_SIZE];
  const char *filterSrcHosts[48];
  const char *filterDstHosts[48];
  const char *filterPktDstHosts[48];
  struct stat statbuf;
  FILE *fp = NULL;
  char *fileName;
  char _filterFilePath[1024];
  json_object *jObj_bpfFilter;
  void *fmap;
  int fsock;
  float average;
  float deviation;
  int duration;
  int typeCheck;
  int array_len;
  int i;

  if((fsock = open(filePath, O_RDONLY)) == -1) {
    fprintf(stderr,"error opening file %s\n", filePath);
    exit(-1);
  }

  if(fstat(fsock, &statbuf) == -1) {
    fprintf(stderr,"error getting file stat\n");
    exit(-1);
  }

  if((fmap = mmap(NULL, statbuf.st_size, PROT_READ, MAP_PRIVATE, fsock, 0)) == MAP_FAILED) {
    fprintf(stderr,"error mmap is failed\n");
    exit(-1);
  }

  if((jObj = json_tokener_parse(fmap)) == NULL) {
    fprintf(stderr,"ERROR: invalid json file. Use -x flag only with .json files generated by ndpiReader -b flag.\n");
    exit(-1);
  }


  if((res = json_object_object_get_ex(jObj, "duration.in.seconds", &jObj_duration)) == 0) {
    fprintf(stderr,"ERROR: can't get \"duration.in.seconds\", use -x flag only with .json files generated by ndpiReader -b flag.\n");
    exit(-1);
  }
  duration = json_object_get_int(jObj_duration);


  if((res = json_object_object_get_ex(jObj, "statistics", &jObj_statistics)) == 0) {
    fprintf(stderr,"ERROR: can't get \"statistics\", use -x flag only with .json files generated by ndpiReader -b flag.\n");
    exit(-1);
  }

  if((typeCheck = json_object_is_type(jObj_statistics, json_type_array)) == 0) {
    fprintf(stderr,"ERROR: invalid json file. Use -x flag only with .json files generated by ndpiReader -b flag.\n");
    exit(-1);
  }
  array_len = json_object_array_length(jObj_statistics);


  bpf_filter_port_array_init(filterSrcPorts, PORT_ARRAY_SIZE);
  bpf_filter_host_array_init(filterSrcHosts, HOST_ARRAY_SIZE);
  bpf_filter_host_array_init(filterDstHosts, HOST_ARRAY_SIZE);
  bpf_filter_host_array_init(filterPktDstHosts, HOST_ARRAY_SIZE/2);

  for(i=0; i<array_len; i++) {
    json_object *stats = json_object_array_get_idx(jObj_statistics, i);
    json_object *val;

    if((res = json_object_object_get_ex(stats, "top.scanner.stats", &val)) == 0) {
      fprintf(stderr,"ERROR: can't get \"top.scanner.stats\", use -x flag only with .json files generated by ndpiReader -b flag.\n");
      exit(-1);
    }

    if((average = getAverage(val, "top.scanner.stats")) != 0){
      deviation = getStdDeviation(val, average, "top.scanner.stats");
      getScannerHosts(val, duration, filterSrcHosts, HOST_ARRAY_SIZE, average+deviation);
    }


    if((res = json_object_object_get_ex(stats, "top.receiver.stats", &val)) == 0) {
      fprintf(stderr,"ERROR: can't get \"top.receiver.stats\", use -x flag only with .json files generated by ndpiReader -b flag.\n");
      exit(-1);
    }
    getReceiverHosts(val, filterPktDstHosts, HOST_ARRAY_SIZE/2);


    if((res = json_object_object_get_ex(stats, "top.src.pkts.stats", &val)) == 0) {
      fprintf(stderr,"ERROR: can't get \"top.src.pkts.stats\", use -x flag only with .json files generated by ndpiReader -b flag.\n");
      exit(-1);
    }

    if((average = getAverage(val, "top.src.pkts.stats")) != 0)
      getSourcePorts(val, filterSrcPorts, PORT_ARRAY_SIZE, average);


    if((res = json_object_object_get_ex(stats, "top.dst.pkts.stats", &val)) == 0) {
      fprintf(stderr,"ERROR: can't get \"top.dst.pkts.stats\", use -x flag only with .json files generated by ndpiReader -b flag.\n");
      exit(-1);
    }
    getDestinationHosts(val, duration, filterDstHosts, HOST_ARRAY_SIZE);
  }


  fileName = basename(filePath);
  snprintf(_filterFilePath, sizeof(_filterFilePath), "%s.bpf", filePath);

  if((fp = fopen(_filterFilePath,"w")) == NULL) {
    printf("Error creating .json file %s\n", _filterFilePath);
    exit(-1);
  }

  jObj_bpfFilter = json_object_new_object();

  bpf_filter_pkt_peak_filter(&jObj_bpfFilter, filterSrcPorts, PORT_ARRAY_SIZE,
			     filterSrcHosts, HOST_ARRAY_SIZE, filterPktDstHosts, HOST_ARRAY_SIZE/2);

  bpf_filter_host_peak_filter(&jObj_bpfFilter, filterDstHosts, HOST_ARRAY_SIZE);

  fprintf(fp,"%s\n",json_object_to_json_string(jObj_bpfFilter));
  fclose(fp);

  printf("created: %s\n", _filterFilePath);

  json_object_put(jObj); /* free memory */
}
#endif

/**
 * Processes all packets assigned to it's buffer
 */
void * thread_waiting_loop(void *_thread_id) {
  long thread_id = (long) _thread_id;
  struct Node *rear; 
  struct Node *front;
  while(keepThreadsRunning) {
    pthread_mutex_t *lock = locks[thread_id];
    pthread_mutex_lock(lock);
    rear = threadQueueRears[thread_id]; 
    front = threadQueueFronts[thread_id];
    if (!isEmpty(&front, &rear)) {
      struct Node* node = dequeue(&front, &rear);
      // printf("dequing from thread: %ld\n", thread_id);
      // printf("lock id: %p\n", lock);
      if (front == NULL) {
          threadQueueRears[thread_id] = rear;
        threadQueueFronts[thread_id] = front;
      } else {
        threadQueueFronts[thread_id] = front;
      }
      pthread_mutex_unlock(lock);
      ndpi_process_packet((u_char*)&thread_id, node);
    } else {
      pthread_mutex_unlock(lock);
    }
  }
  return NULL;
}

/**
 * starts functionality of distribution of packets to different threads according to their hash value
 */
void * distributePacketsThread(void *_thread_id) {
  printf("distributePacketsThread\n");
#ifdef USE_DPDK
  long thread_id = (long) _thread_id;
  // next line is temporary
  thread_id = 0;
  processing_thread((void *)thread_id);
  keepThreadsRunning = 0;
  return NULL;
#else
  long thread_id = (long) _thread_id;
  int setUpDone = 0;
  for(thread_id = 0; thread_id < num_pcap_files; thread_id++) {
    pcap_t *cap;

    #ifdef DEBUG_TRACE
        if(trace) fprintf(trace, "Opening %s\n", (const u_char*)_pcap_file[thread_id]);
    #endif

    if (setUpDone == 0) {
      cap = openPcapFileOrDevice(thread_id, (const u_char*)_pcap_file[thread_id]);
      for (int thread = 0; thread < num_threads; thread++) {
        setupDetection(thread, cap);
      }
      setUpDone = 1;
    } else {
      pcap_close(ndpi_thread_info[0].workflow->pcap_handle);
      cap = openPcapFileOrDevice(thread_id, (const u_char*)_pcap_file[thread_id]);
      for (int thread = 0; thread < num_threads; thread++) {
        ndpi_thread_info[thread].workflow->pcap_handle = cap;
      }
    }
    // TEMPORARY CHANGE
    long thread_id_workflow = 0;
    processing_thread((void *)thread_id_workflow);
  }
  pcap_close(ndpi_thread_info[0].workflow->pcap_handle);
  keepThreadsRunning = 0;
  return NULL;
#endif
}

/**\
 * Starts pthreads
 */
void start_threads() {
  long thread_id;
  int status;
  void * thd_res;
  
  /* Running processing threads */
  for(thread_id = 0; thread_id < num_threads; thread_id++) {
    status = pthread_create(&ndpi_thread_info[thread_id].pthread, NULL, thread_waiting_loop, (void *) thread_id);
    printf("Processing thread %ld is running\n", thread_id);
    /* check pthreade_create return value */
    if(status != 0) {
      fprintf(stderr, "error on create %ld thread\n", thread_id);
      exit(-1);
    }
  }
  distributePacketsThread((void *) thread_id);

  /* Waiting for completion of processing threads*/
  for(thread_id = 0; thread_id < num_threads; thread_id++) {
    status = pthread_join(ndpi_thread_info[thread_id].pthread, &thd_res);
    /* check pthreade_join return value */
    if(status != 0) {
      printf("return value of %d\n", status);
      exit(-1);
    }
    if(thd_res != NULL) {
      fprintf(stderr, "error on returned value of %ld joined thread\n", thread_id);
      exit(-1);
    }
  }
}

/**
   @brief MAIN FUNCTION
**/
#ifdef APP_HAS_OWN_MAIN
int orginal_main(int argc, char **argv) {
#else
  int main(int argc, char **argv) {
#endif
    int i;  
    if(ndpi_get_api_version() != NDPI_API_VERSION) {
      printf("nDPI Library version mismatch: please make sure this code and the nDPI library are in sync\n");
      return(-1);
    }

    gettimeofday(&startProg, NULL);

    automataUnitTest();

    gettimeofday(&startup_time, NULL);
    ndpi_info_mod = ndpi_init_detection_module();
	   
    if(ndpi_info_mod == NULL) return -1;

    memset(ndpi_thread_info, 0, sizeof(ndpi_thread_info));

    parseOptions(argc, argv);
    
    /* Create an array within which to put pointers to the front and rear nodes of the corresponding queues (linked lists) for processing */
    threadQueueRears = malloc(sizeof(*threadQueueRears) * num_threads);
    threadQueueFronts = malloc(sizeof(*threadQueueFronts) * num_threads);
    for (int i = 0; i < num_threads; i++) {
      threadQueueRears[i] = NULL;
      threadQueueFronts[i] = NULL;
    }

    /* Create an array within which to put the pointers to the locks associated with each processing thread */
    locks = malloc(sizeof(*locks) * num_threads);
    for (int i = 0; i < num_threads; i++) {
      pthread_mutex_t *lock;
      lock = malloc(sizeof(pthread_mutex_t));
      if (pthread_mutex_init(lock, NULL) != 0)
      {
          printf("\n mutex init failed\n");
          return 1;
      }
      locks[i] = lock;
    }
     
  
    if(bpf_filter_flag) {
#ifdef HAVE_JSON_C
      produceBpfFilter(_diagnoseFilePath);
      return 0;
#endif
    }

    if((!json_flag) && (!quiet_mode)) {
      printf("\n-----------------------------------------------------------\n"
	     "* NOTE: This is demo app to show *some* nDPI features.\n"
	     "* In this demo we have implemented only some basic features\n"
	     "* just to show you what you can do with the library. Feel \n"
	     "* free to extend it and send us the patches for inclusion\n"
	     "------------------------------------------------------------\n\n");

      printf("Using nDPI (%s) [%d thread(s)]\n", ndpi_revision(), num_threads);
    }

    signal(SIGINT, sigproc);
     
    test_lib();
    if(results_path)  free(results_path);
    if(results_file)  fclose(results_file);
    if(extcap_dumper) pcap_dump_close(extcap_dumper);
    if(ndpi_info_mod) ndpi_exit_detection_module(ndpi_info_mod);

    // pthread_mutex_destroy(&lock);
    for (int i = 0; i < num_threads; i++) {
      pthread_mutex_t *lock = locks[i];
      pthread_mutex_destroy(lock);
    }

    gettimeofday(&endProg, NULL);
    printf("Time taken to run program: %ld\n", endProg.tv_sec - startProg.tv_sec);
  }

#ifdef WIN32
#ifndef __GNUC__
#define EPOCHFILETIME (116444736000000000i64)
#else
#define EPOCHFILETIME (116444736000000000LL)
#endif

/**
   @brief Timezone
**/
  struct timezone {
    int tz_minuteswest; /* minutes W of Greenwich */
    int tz_dsttime;     /* type of dst correction */
  };


/**
   @brief Set time
**/
  int gettimeofday(struct timeval *tv, struct timezone *tz) {
    FILETIME        ft;
    LARGE_INTEGER   li;
    __int64         t;
    static int      tzflag;

    if(tv) {
      GetSystemTimeAsFileTime(&ft);
      li.LowPart  = ft.dwLowDateTime;
      li.HighPart = ft.dwHighDateTime;
      t  = li.QuadPart;       /* In 100-nanosecond intervals */
      t -= EPOCHFILETIME;     /* Offset to the Epoch time */
      t /= 10;                /* In microseconds */
      tv->tv_sec  = (long)(t / 1000000);
      tv->tv_usec = (long)(t % 1000000);
    }

    if(tz) {
      if(!tzflag) {
	_tzset();
	tzflag++;
      }

      tz->tz_minuteswest = _timezone / 60;
      tz->tz_dsttime = _daylight;
    }

    return 0;
  }
#endif /* WIN32 */
