/*
 *   This file is part of ubridge, a program to bridge network interfaces
 *   to UDP tunnels.
 *
 *   Copyright (C) 2015 GNS3 Technologies Inc.
 *
 *   ubridge is free software: you can redistribute it and/or modify it
 *   under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   ubridge is distributed in the hope that it will be useful, but
 *   WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <pthread.h>

#include "ubridge.h"
#include "parse.h"
#include "pcap_capture.h"
#include "hypervisor.h"
#ifdef __linux__
#include "hypervisor_iol_bridge.h"
#endif

char *config_file = CONFIG_FILE;
pthread_mutex_t global_lock = PTHREAD_MUTEX_INITIALIZER;
bridge_t *bridge_list = NULL;
int debug_level = 0;
int hypervisor_mode = 0;

static void bridge_nios(nio_t *source_nio, nio_t *destination_nio, bridge_t *bridge)
{
  ssize_t bytes_received, bytes_sent;
  unsigned char pkt[NIO_MAX_PKT_SIZE];

  while (1) {

    /* received from the source NIO */
    bytes_received = nio_recv(source_nio, &pkt, NIO_MAX_PKT_SIZE);
    if (bytes_received == -1) {
        if (errno == ECONNREFUSED || errno == ENETDOWN)
           continue;
        perror("recv");
        break;
    }

    if (bytes_received > NIO_MAX_PKT_SIZE) {
        fprintf(stderr, "received frame is %zd bytes (maximum is %d bytes)\n", bytes_received, NIO_MAX_PKT_SIZE);
        continue;
    }

    source_nio->bytes_sent += bytes_received;

    if (debug_level > 0) {
        if (source_nio == bridge->source_nio)
           printf("Received %zd bytes on bridge %s (source NIO)\n", bytes_received, bridge->name);
        else
           printf("Received %zd bytes on bridge %s (destination NIO)\n", bytes_received, bridge->name);
        if (debug_level > 1)
            dump_packet(stdout, pkt, bytes_received);
    }

    /* dump the packet to a PCAP file if capture is activated */
    pcap_capture_packet(bridge->capture, pkt, bytes_received);

    /* send what we received to the destination NIO */
    bytes_sent = nio_send(destination_nio, pkt, bytes_received);
    if (bytes_sent == -1) {
        if (errno == ECONNREFUSED || errno == ENETDOWN)
           continue;

        /* The linux tap driver returns EIO if the device is not up. From the ubridge side this is not an error, so we should ignore it. */
        if (destination_nio->type == NIO_TYPE_TAP && errno == EIO)
            continue;

        perror("send");
        break;
    }
    destination_nio->bytes_received += bytes_sent;
  }
}

/* Source NIO thread */
void *source_nio_listener(void *data)
{
  bridge_t *bridge = data;

  printf("Source NIO listener thread for %s has started\n", bridge->name);
  if (bridge->source_nio && bridge->destination_nio)
    /* bridges from the source NIO to the destination NIO */
    bridge_nios(bridge->source_nio, bridge->destination_nio, bridge);
  printf("Source NIO listener thread for %s has stopped\n", bridge->name);
  pthread_exit(NULL);
}

/* Destination NIO thread */
void *destination_nio_listener(void *data)
{
  bridge_t *bridge = data;

  printf("Destination NIO listener thread for %s has started\n", bridge->name);
  if (bridge->source_nio && bridge->destination_nio)
      /* bridges from the destination NIO to the source NIO */
      bridge_nios(bridge->destination_nio, bridge->source_nio, bridge);
  printf("Destination NIO listener thread for %s has stopped\n", bridge->name);
  pthread_exit(NULL);
}

static void free_bridges(bridge_t *bridge)
{
  bridge_t *next;

  while (bridge != NULL) {
    if (bridge->name)
       free(bridge->name);
    pthread_cancel(bridge->source_tid);
    pthread_join(bridge->source_tid, NULL);
    pthread_cancel(bridge->destination_tid);
    pthread_join(bridge->destination_tid, NULL);
    free_nio(bridge->source_nio);
    free_nio(bridge->destination_nio);
    free_pcap_capture(bridge->capture);
    next = bridge->next;
    free(bridge);
    bridge = next;
  }
}

#ifdef __linux__
static void free_iol_bridges(iol_bridge_t *bridge)
{
  iol_bridge_t *next;
  int i;

  while (bridge != NULL) {
    if (bridge->name)
       free(bridge->name);

    close(bridge->iol_bridge_sock);
    unlink(bridge->bridge_sockaddr.sun_path);
    if ((unlock_unix_socket(bridge->sock_lock, bridge->bridge_sockaddr.sun_path)) == -1)
       fprintf(stderr, "failed to unlock %s\n", bridge->bridge_sockaddr.sun_path);

    pthread_cancel(bridge->bridge_tid);
    pthread_join(bridge->bridge_tid, NULL);
    for (i = 0; i < MAX_PORTS; i++) {
        if (bridge->port_table[i].destination_nio != NULL) {
           pthread_cancel(bridge->port_table[i].tid);
           pthread_join(bridge->port_table[i].tid, NULL);
           free_pcap_capture(bridge->port_table[i].capture);
           free_nio(bridge->port_table[i].destination_nio);
        }
    }
    free(bridge->port_table);
    next = bridge->next;
    free(bridge);
    bridge = next;
  }
}
#endif

static void create_threads(bridge_t *bridge)
{
    int s;

    while (bridge != NULL) {
       s = pthread_create(&(bridge->source_tid), NULL, &source_nio_listener, bridge);
       if (s != 0)
         handle_error_en(s, "pthread_create");
       s = pthread_create(&(bridge->destination_tid), NULL, &destination_nio_listener, bridge);
       if (s != 0)
         handle_error_en(s, "pthread_create");
       bridge = bridge->next;
    }
}

void ubridge_reset()
{
   free_bridges(bridge_list);
#ifdef __linux__
   free_iol_bridges(iol_bridge_list);
#endif
}

/* Generic signal handler */
void signal_gen_handler(int sig)
{
   switch(sig) {
      case SIGTERM:
      case SIGINT:
         /* CTRL+C has been pressed */
         if (hypervisor_mode)
            hypervisor_stopsig();
         break;
#ifndef CYGWIN
         /* CTRL+C has been pressed */
      case SIGHUP:
         break;
#endif
      default:
         fprintf(stderr, "Unhandled signal %d\n", sig);
   }
}

static void ubridge(char *hypervisor_ip_address, int hypervisor_tcp_port)
{
   if (hypervisor_mode) {
       struct sigaction act;

       memset(&act, 0, sizeof(act));
       act.sa_handler = signal_gen_handler;
       act.sa_flags = SA_RESTART;
#ifndef CYGWIN
       sigaction(SIGHUP, &act, NULL);
#endif
       sigaction(SIGTERM, &act, NULL);
       sigaction(SIGINT, &act, NULL);
       sigaction(SIGPIPE, &act, NULL);

      run_hypervisor(hypervisor_ip_address, hypervisor_tcp_port);
      free_bridges(bridge_list);
#ifdef __linux__
      free_iol_bridges(iol_bridge_list);
#endif
   }
   else {
      sigset_t sigset;
      int sig;

      sigemptyset(&sigset);
      sigaddset(&sigset, SIGINT);
      sigaddset(&sigset, SIGTERM);
#ifndef CYGWIN
      sigaddset(&sigset, SIGHUP);
#endif
      pthread_sigmask(SIG_BLOCK, &sigset, NULL);

      while (1) {
         if (!parse_config(config_file, &bridge_list))
            break;
         create_threads(bridge_list);
         sigwait(&sigset, &sig);

         free_bridges(bridge_list);
         if (sig == SIGTERM || sig == SIGINT)
            break;
         printf("Reloading configuration\n");
     }
   }
}

/* Display all network devices on this host */
static void display_network_devices(void)
{
   char pcap_errbuf[PCAP_ERRBUF_SIZE];
   pcap_if_t *device_list, *device;
   int res;

   printf("Network device list:\n\n");

#ifndef CYGWIN
   res = pcap_findalldevs(&device_list, pcap_errbuf);
#else
   res = pcap_findalldevs_ex(PCAP_SRC_IF_STRING,NULL, &device_list, pcap_errbuf);
#endif

   if (res < 0) {
      fprintf(stderr, "PCAP: unable to find device list (%s)\n", pcap_errbuf);
      return;
   }

   for(device = device_list; device; device = device->next)
      printf("  %s => %s\n", device->name, device->description ? device->description : "no description");
   printf("\n");

   pcap_freealldevs(device_list);
}

static void print_usage(const char *program_name)
{
  printf("Usage: %s [OPTION]\n"
         "\n"
         "Options:\n"
         "  -h                           : Print this message and exit\n"
         "  -f <file>                    : Specify a INI configuration file (default: %s)\n"
         "  -H [<ip_address>:]<tcp_port> : Run in hypervisor mode\n"
         "  -e                           : Display all available network devices and exit\n"
         "  -d <level>                   : Debug level\n"
         "  -v                           : Print version and exit\n",
         program_name,
         CONFIG_FILE);
}

int main(int argc, char **argv)
{
  int hypervisor_tcp_port = 0;
  char *hypervisor_ip_address = NULL;
  char opt;
  char *index;
  size_t len;

  while ((opt = getopt(argc, argv, "hved:f:H:")) != -1) {
    switch (opt) {
      case 'H':
        hypervisor_mode = 1;
        index = strrchr(optarg, ':');
        if (!index) {
           hypervisor_tcp_port = atoi(optarg);
        } else {
           len = index - optarg;
           hypervisor_ip_address = realloc(hypervisor_ip_address, len + 1);

           if (!hypervisor_ip_address) {
              fprintf(stderr, "Unable to set hypervisor IP address!\n");
              exit(EXIT_FAILURE);
           }
           memcpy(hypervisor_ip_address, optarg, len);
           hypervisor_ip_address[len] = '\0';
           hypervisor_tcp_port = atoi(index + 1);
        }
        break;
	  case 'v':
	    printf("%s version %s\n", NAME, VERSION);
	    exit(EXIT_SUCCESS);
	  case 'h':
	    print_usage(argv[0]);
	    exit(EXIT_SUCCESS);
	  case 'e':
	    display_network_devices();
	    exit(EXIT_SUCCESS);
	  case 'd':
        debug_level = atoi(optarg);
        break;
	  case 'f':
        config_file = optarg;
        break;
      default:
        exit(EXIT_FAILURE);
	}
  }
  ubridge(hypervisor_ip_address, hypervisor_tcp_port);
  return (EXIT_SUCCESS);
}
