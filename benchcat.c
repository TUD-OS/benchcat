/* -*- Mode: C -*- */

/*
 * Benchcat
 *
 * Copyright (C) 2012, Julian Stecklina <jsteckli@os.inf.tu-dresden.de>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * This file is part of Vancouver.
 *
 * Benchcat is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Benchcat is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License version 2 for more details.
 */


#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>

static uint64_t bytes_per_second;
static uint16_t external_port;

static pthread_mutex_t budget_mtx;

static uint8_t buffer[1024*1024];

static void
print_help(void)
{
  printf("Usage: limit-in-mbit port\n");
}

static struct timespec last_call;

static uint32_t get_budget()
{
 again:
  //uint64_t max_chunk = bytes_per_second / FREQUENCY;
  pthread_mutex_lock(&budget_mtx);

  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  
  uint64_t diff = ((uint64_t)now.tv_sec - (uint64_t)last_call.tv_sec) * 1000000000ULL + ((uint64_t)now.tv_nsec - (uint64_t)last_call.tv_nsec);

  double budget = (double)bytes_per_second * (double)diff /* ns */ / 1000000000.0d;

  if (budget < 1500)
    goto sleep;

  /* printf("%llu ns -> %lf bytes budget (%llu bps)\n", diff, budget, bytes_per_second); */
  last_call = now;

  pthread_mutex_unlock(&budget_mtx);

  if (budget > 10000000)
    budget = 10000000;

  return budget;

 sleep:
  pthread_mutex_unlock(&budget_mtx);
  usleep(1000);
  goto again;
}

static void *handler_fn(void *p)
{
  int sock = (uintptr_t)p;

  if (shutdown(sock, SHUT_RD) < 0) { perror("shutdown"); goto close_it; }

  /* if (fcntl(sock, F_SETFL, (long)O_NONBLOCK) < 0) { */
  /*   perror("fcntl"); goto close_it; */
  /* } */

  while (1) {
    uint32_t budget = get_budget();

    while (budget) {
      uint32_t chunk = (budget < sizeof(buffer)) ? budget : sizeof(buffer);
      ssize_t written = 0;
      if ((written = write(sock, buffer, chunk)) <= 0)
        goto close_it;
      //printf("Written %zu\n", written);
      budget -= written;
    }
  }

 close_it:
  perror("write");
  close(sock);
  return NULL;
}

int main(int argc, char **argv)
{
  if (pthread_mutex_init(&budget_mtx, NULL) < 0) {
    perror("pthread_mutex_init"); return EXIT_FAILURE;
  }

  /* Ignore SIGPIPE, otherwise we will die when the remote end closes
     a connection. */
  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) { perror("signal"); return EXIT_FAILURE; };

  if (argc != 3) {
    print_help();
    return EXIT_FAILURE;
  }
  
  bytes_per_second = strtoull(argv[1], NULL, 0) * 1000 * 1000 / 8;
  external_port    = strtoul(argv[2], NULL, 0);

  printf("Output %" PRIu64 " MBit/s on port %u.\n", 8*bytes_per_second / (1000 * 1000), external_port);

  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) { perror("socket"); return EXIT_FAILURE; }


  /* Enable SO_REUSEADDR */
  int optval = 1;
  if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
    perror("setsockopt"); return EXIT_FAILURE;
  }

  struct sockaddr_in my_addr, peer_addr;
  memset(&my_addr,   0, sizeof(my_addr));
  memset(&peer_addr, 0, sizeof(peer_addr));

  my_addr.sin_family = AF_INET;
  my_addr.sin_port   = htons(external_port);
  my_addr.sin_addr.s_addr = htonl (INADDR_ANY);
  
  if (bind(sock, (struct sockaddr *)&my_addr, sizeof(my_addr)) < 0) {
    perror("bind"); return EXIT_FAILURE;
  }

  if (listen(sock, 10) < 0) { perror("listen"); return EXIT_FAILURE; }

  int psock;
  socklen_t addrlen = sizeof(peer_addr);
  while ((psock = accept(sock, (struct sockaddr *)&peer_addr, &addrlen)) >= 0) {
    printf("Accepted connection.\n");
    addrlen = sizeof(peer_addr);
    pthread_t p;
    if (pthread_create(&p, NULL, handler_fn, (void *)(uintptr_t)psock) != 0) {
      perror("pthread_create"); return EXIT_FAILURE;
    }
  }

  perror("accept");
  return EXIT_FAILURE;
}
