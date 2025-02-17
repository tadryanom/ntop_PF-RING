/*
 * (C) 2003-23 - ntop 
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#define _GNU_SOURCE
#include <signal.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <sys/time.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>

#include "pfring.h"
#include "pfring_zc.h"

#include "zutils.c"

#define ALARM_SLEEP             1
#define MAX_CARD_SLOTS      32768
#define CACHE_LINE_LEN         64

//#define USE_BURST_API
#define BURST_LEN   32

pfring_zc_cluster *zc;
pfring_zc_queue *zq;
pfring_zc_pkt_buff *buffers[BURST_LEN];

struct timeval startTime;
unsigned long long numPkts = 0, numBytes = 0;
int bind_core = -1;
int bind_time_pulse_core = -1;
int buffer_len;
u_int8_t wait_for_packet = 1, do_shutdown = 0, verbose = 0, add_filtering_rule = 0;
u_int8_t high_stats_refresh = 0, time_pulse = 0, touch_payload = 0;

u_int64_t prev_ns = 0;
u_int64_t threshold_min = 1500, threshold_max = 2500; /* TODO parameters */
u_int64_t threshold_min_count = 0, threshold_max_count = 0;

volatile u_int64_t *pulse_timestamp_ns;

/* ******************************** */

void *time_pulse_thread(void *data) {
  struct timespec tn;

  bind2core(bind_time_pulse_core);

  while (likely(!do_shutdown)) {
    /* clock_gettime takes up to 30 nsec to get the time */
    clock_gettime(CLOCK_REALTIME, &tn);
    *pulse_timestamp_ns = ((u_int64_t) ((u_int64_t) tn.tv_sec * 1000000000) + tn.tv_nsec);
  }

  return NULL;
}

/* ******************************** */

void print_stats() {
  struct timeval endTime;
  double deltaMillisec;
  static u_int8_t print_all;
  static u_int64_t lastPkts = 0;
  static u_int64_t lastDrops = 0;
  static u_int64_t lastBytes = 0;
  double pktsDiff, dropsDiff, bytesDiff;
  static struct timeval lastTime;
  char buf1[64], buf2[64], buf3[64];
  unsigned long long nBytes = 0, nPkts = 0, nDrops = 0;
  pfring_zc_stat stats;

  if(startTime.tv_sec == 0) {
    gettimeofday(&startTime, NULL);
    print_all = 0;
  } else
    print_all = 1;

  gettimeofday(&endTime, NULL);
  deltaMillisec = delta_time(&endTime, &startTime);

  nBytes = numBytes;
  nPkts = numPkts;
  if (pfring_zc_stats(zq, &stats) == 0)
    nDrops = stats.drop;

  fprintf(stderr, "=========================\n"
	  "Absolute Stats: %s pkts (%s drops) - %s bytes\n", 
	  pfring_format_numbers((double)nPkts, buf1, sizeof(buf1), 0),
	  pfring_format_numbers((double)nDrops, buf3, sizeof(buf3), 0),
	  pfring_format_numbers((double)nBytes, buf2, sizeof(buf2), 0));

  if(print_all && (lastTime.tv_sec > 0)) {
    char buf[256];

    deltaMillisec = delta_time(&endTime, &lastTime);
    pktsDiff = nPkts-lastPkts;
    dropsDiff = nDrops-lastDrops;
    bytesDiff = nBytes - lastBytes;
    bytesDiff /= (1000*1000*1000)/8;

    if (time_pulse)
      fprintf(stderr, "Thresholds: %ju pkts <%.3fusec %ju pkts >%.3fusec\n", 
        threshold_min_count, (double) threshold_min/1000, 
        threshold_max_count, (double) threshold_max/1000);

    snprintf(buf, sizeof(buf),
	     "Actual Stats: %s pps (%s drops) - %s Gbps",
	     pfring_format_numbers(((double)pktsDiff/(double)(deltaMillisec/1000)),  buf1, sizeof(buf1), 1),
	     pfring_format_numbers(((double)dropsDiff/(double)(deltaMillisec/1000)),  buf2, sizeof(buf2), 1),
	     pfring_format_numbers(((double)bytesDiff/(double)(deltaMillisec/1000)),  buf3, sizeof(buf3), 1));
    fprintf(stderr, "%s\n", buf);
  }
    
  fprintf(stderr, "=========================\n\n");

  lastPkts = nPkts, lastDrops = nDrops, lastBytes = nBytes;
  lastTime.tv_sec = endTime.tv_sec, lastTime.tv_usec = endTime.tv_usec;
}

/* ******************************** */

void sigproc(int sig) {
  static int called = 0;
  fprintf(stderr, "Leaving...\n");
  if(called) return; else called = 1;

  do_shutdown = 1;

  print_stats();
  
  pfring_zc_queue_breakloop(zq);
}

/* *************************************** */

void printHelp(void) {
  printf("zcount - (C) 2014-23 ntop\n");
  printf("Using PFRING_ZC v.%s\n", pfring_zc_version());
  printf("A simple packet counter application.\n\n");
  printf("Usage:   zcount -i <device> -c <cluster id>\n"
	 "                [-h] [-g <core id>] [-R] [-H] [-S <core id>] [-v] [-a] [-t]\n\n");
  printf("-h              Print this help\n");
  printf("-i <device>     Device name\n");
  printf("-c <cluster id> Cluster id\n");
  printf("-g <core id>    Bind this app to a core\n");
  printf("-a              Active packet wait\n");
  printf("-f <bpf>        Set a BPF filter\n");
  printf("-R              Test hw filters adding a rule (Intel 82599)\n");
  printf("-H              High stats refresh rate (workaround for drop counter on 1G Intel cards)\n");
  printf("-X              Enable hardware timestamp (when supported)\n");
  printf("-s <time>       Set hardware timestamp (when supported). Format example: '2022-09-23 14:30:55.123456789'\n");
  printf("-d <nsec>       Adjust hardware timestamp using a signed nsec delta (when supported)'\n");
  printf("-S <core id>    Pulse-time thread for inter-packet time check\n");
  printf("-T              Capture also TX (standard kernel drivers only)\n");
  printf("-t              Touch payload (to force packet load on cache)\n");
  printf("-D              Debug mode\n");
  printf("-C              Check license\n");
  printf("-M              Print maintenance\n");
  printf("-v <level>      Verbose (1 to print packet headers, 2 to print hex)\n");
}

/* *************************************** */

void print_packet(pfring_zc_pkt_buff *buffer) {
  u_char *pkt_data = pfring_zc_pkt_buff_data(buffer, zq);

  if (buffer->ts.tv_sec)
    printf("[%u.%u] ", buffer->ts.tv_sec, buffer->ts.tv_nsec);

  if (buffer->hash)
    printf("[hash=%08X] ", buffer->hash);

  if (verbose == 1) {
    char bigbuf[4096];
    pfring_print_pkt(bigbuf, sizeof(bigbuf), pkt_data, buffer->len, buffer->len);
    fputs(bigbuf, stdout);
  } else {
    int i;
    for(i = 0; i < buffer->len; i++)
      printf("%02X ", pkt_data[i]);
    printf("\n");
  }
}

/* *************************************** */


void *packet_consumer_thread(void *user) {
#ifdef USE_BURST_API
  int i, n;
#endif

  if (bind_core >= 0)
    bind2core(bind_core);

  while(!do_shutdown) {
#ifdef USE_BURST_API
    if((n = pfring_zc_recv_pkt_burst(zq, buffers, BURST_LEN, wait_for_packet)) > 0) {

      if (unlikely(verbose))
        for (i = 0; i < n; i++) 
          print_packet(buffers[i]);

      for (i = 0; i < n; i++) {
        numPkts++;
        numBytes += buffers[i]->len + 24; /* 8 Preamble + 4 CRC + 12 IFG */
      }
    }
#else
    if(pfring_zc_recv_pkt(zq, &buffers[0], wait_for_packet) > 0) {

      if (unlikely(time_pulse)) {
        u_int64_t now_ns = *pulse_timestamp_ns;
        u_int64_t diff_ns = now_ns - prev_ns;
        if (diff_ns < threshold_min) threshold_min_count++;
        else if (diff_ns > threshold_max) threshold_max_count++;
        prev_ns = now_ns;
      }

      if(touch_payload) {
	u_char *p = pfring_zc_pkt_buff_data(buffers[0], zq);
	volatile int __attribute__ ((unused)) i;
	
	i = p[12] + p[13];
      }

      if (unlikely(verbose))
        print_packet(buffers[0]);

      numPkts++;
      numBytes += buffers[0]->len + 24; /* 8 Preamble + 4 CRC + 12 IFG */
    }
#endif
  }

   pfring_zc_sync_queue(zq, rx_only);

  return NULL;
}

/* *************************************** */

int main(int argc, char* argv[]) {
  char *device = NULL, c;
  int i, cluster_id = DEFAULT_CLUSTER_ID, rc = 0, check_license = 0, print_maintenance = 0;
  pthread_t my_thread;
  struct timeval timeNow, lastTime;
  pthread_t time_thread;
  u_int32_t flags;
  char *filter = NULL;
  char *init_time = NULL;
  long long shift_time = 0;

  lastTime.tv_sec = 0;
  startTime.tv_sec = 0;

  flags = PF_RING_ZC_DEVICE_CAPTURE_INJECTED;

  while((c = getopt(argc,argv,"ac:d:f:g:hi:v:CDMRHs:S:TtX")) != '?') {
    if((c == 255) || (c == -1)) break;

    switch(c) {
    case 'h':
      printHelp();
      exit(0);
      break;
    case 'a':
      wait_for_packet = 0;
      break;
    case 'c':
      cluster_id = atoi(optarg);
      break;
    case 'd':
      flags |= PF_RING_ZC_DEVICE_HW_TIMESTAMP;
      shift_time = atoll(optarg);
      break;
    case 'f':
      filter = strdup(optarg);
      break;
    case 'i':
      device = strdup(optarg);
      break;
    case 'g':
      bind_core = atoi(optarg);
      break;
    case 'R':
      add_filtering_rule = 1;
      break;
    case 'H':
      high_stats_refresh = 1;
      break;
    case 's':
      flags |= PF_RING_ZC_DEVICE_HW_TIMESTAMP;
      init_time = strdup(optarg);
      break;
    case 'S':
      time_pulse = 1;
      bind_time_pulse_core = atoi(optarg);
      break;
    case 'T':
      flags |= PF_RING_ZC_DEVICE_CAPTURE_TX;
      break;
    case 't':
      touch_payload = 1;
      break;
    case 'v':
      verbose = atoi(optarg);
      break;
    case 'C':
      check_license = 1;
      break;
    case 'D':
      pfring_zc_debug();
      break;
    case 'M':
      print_maintenance = 1;
      break;
    case 'X':
      flags |= PF_RING_ZC_DEVICE_HW_TIMESTAMP;
      break;
    }
  }
  
  if (device == NULL || cluster_id < 0) {
    printHelp();
    exit(-1);
  }

  buffer_len = max_packet_len(device);

  zc = pfring_zc_create_cluster(
    cluster_id, 
    buffer_len,
    0, 
    MAX_CARD_SLOTS + BURST_LEN,
    pfring_zc_numa_get_cpu_node(bind_core),
    NULL /* auto hugetlb mountpoint */,
    0 
  );

  if(zc == NULL) {
    fprintf(stderr, "pfring_zc_create_cluster error [%s] Please check that pf_ring.ko is loaded and hugetlb fs is mounted\n",
	    strerror(errno));
    return -1;
  }

  /* Read memory info:
  pfring_zc_cluster_mem_info mem_info;
  pfring_zc_get_memory_info(zc, &mem_info);
  printf("Base address: %p Size: %ju\n", mem_info.base_addr, mem_info.size);
  */

  zq = pfring_zc_open_device(zc, device, rx_only, flags);

  if(zq == NULL) {
    fprintf(stderr, "pfring_zc_open_device error [%s] Please check that %s is up and not already used\n",
	    strerror(errno), device);
    rc = -1;
    goto cleanup;
  }

  if (check_license || print_maintenance) {
    if (strncmp(device, "zc:", 3) == 0) {
      u_int32_t maintenance;
      if (pfring_zc_check_device_license(zq, &maintenance)) {
        if (print_maintenance) {
          time_t exp = maintenance;
          printf("%u %s\n", maintenance, maintenance > 0 ? ctime(&exp) : "No expiration");
        } else /* check_license only */ {
          printf("License Ok\n");
        }
      } else {
        printf("Invalid license\n");
      }
    } else {
      fprintf(stderr, "This does not look like an Intel ZC interface. Please check pfcount -L -v 1\n");
    }
    goto cleanup;
  }

  for (i = 0; i < BURST_LEN; i++) { 

    buffers[i] = pfring_zc_get_packet_handle(zc);

    if (buffers[i] == NULL) {
      fprintf(stderr, "pfring_zc_get_packet_handle error\n");
      rc = -1;
      goto cleanup;
    }
  }

  if (filter != NULL) {
    if (pfring_zc_set_bpf_filter(zq, filter) != 0) {
      fprintf(stderr, "pfring_zc_set_bpf_filter error setting '%s'\n", filter);
      return -1;
    }
  }

  if(add_filtering_rule) {
    int rc;
    hw_filtering_rule rule;
    intel_82599_perfect_filter_hw_rule *perfect_rule = &rule.rule_family.perfect_rule;

    memset(&rule, 0, sizeof(rule)), rule.rule_family_type = intel_82599_perfect_filter_rule;
    rule.rule_id = 0, perfect_rule->queue_id = -1, perfect_rule->proto = 17, perfect_rule->s_addr = ntohl(inet_addr("10.0.0.1"));

    rc = pfring_zc_add_hw_rule(zq, &rule);

    if(rc != 0)
      printf("pfring_zc_add_hw_rule(%d) failed: did you enable the FlowDirector (ethtool -K ethX ntuple on)\n", rule.rule_id);
    else
      printf("pfring_zc_add_hw_rule(%d) succeeded: dropping UDP traffic 192.168.30.207:* -> *\n", rule.rule_id);
  }

  signal(SIGINT,  sigproc);
  signal(SIGTERM, sigproc);
  signal(SIGINT,  sigproc);

  if (time_pulse) {
    pulse_timestamp_ns = calloc(CACHE_LINE_LEN/sizeof(u_int64_t), sizeof(u_int64_t));
    pthread_create(&time_thread, NULL, time_pulse_thread, NULL);
    while (!*pulse_timestamp_ns && !do_shutdown); /* wait for ts */
  }

  if (init_time) {
    int rc;
    struct timespec ts;

    rc = str2nsec(init_time, &ts);

    if (rc == 0)
      rc = pfring_zc_set_device_clock(zq, &ts);

    if (rc == 0) printf("Device clock correctly initialized\n");
    else printf("Unable to set device clock (%u)\n", rc);
  }

  if (shift_time) {
    int rc;
    struct timespec ts;
    int sign = 0;

    if (shift_time < 0) {
      sign = -1;
      shift_time = -shift_time;
    }

    ts.tv_sec  = shift_time / 1000000000;
    ts.tv_nsec = shift_time % 1000000000;

    rc = pfring_zc_adjust_device_clock(zq, &ts, sign);

    if (rc == 0) printf("Device clock adjusted (%s %ld.%ld)\n", sign ? "-" : "+", ts.tv_sec, ts.tv_nsec);
    else printf("Unable to adjust device clock (%u)\n", rc);
  }

  pthread_create(&my_thread, NULL, packet_consumer_thread, (void*) NULL);

  if (!verbose) while (!do_shutdown) {
    if (high_stats_refresh) {
      pfring_zc_stat stats;

      pfring_zc_stats(zq, &stats);
      gettimeofday(&timeNow, NULL);

      if (timeNow.tv_sec != lastTime.tv_sec) {
        lastTime.tv_sec = timeNow.tv_sec;
        print_stats();
      }
      usleep(1);
    } else {
      sleep(ALARM_SLEEP);
      print_stats();
    }
  }

  pthread_join(my_thread, NULL);

  sleep(1);

  if (time_pulse)
    pthread_join(time_thread, NULL);

cleanup:

  pfring_zc_destroy_cluster(zc);

  return rc;
}

