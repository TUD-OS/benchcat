/* -*- Mode: C -*- */

/*
 * Benchcat
 *
 * Copyright (C) 2012, Julian Stecklina <jsteckli@os.inf.tu-dresden.de>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * This file is part of benchcat.
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

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
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
#include <errno.h>
#include <sys/errno.h>

#ifdef __linux__
# include <sys/sendfile.h>
#elif __FreeBSD__
# include <sys/uio.h>
#else
# error "Your OS is not supported. Implement sendfile."
#endif

#define MAX_CHUNK (2*1024*1024)

static uint64_t bytes_per_second;
static uint16_t external_port;
static int      devzero;
static bool     receiving = true;

static pthread_mutex_t budget_mtx;

static void
print_help(void)
{
  fprintf(stderr, "Usage: limit-in-mbit port send/recv\n");
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

  double budget = (double)bytes_per_second * (double)diff /* ns */ / 1000000000.0l;

  if (budget < 1500)
    goto sleep;

  /* printf("%llu ns -> %lf bytes budget (%llu bps)\n", diff, budget, bytes_per_second); */
  last_call = now;

  pthread_mutex_unlock(&budget_mtx);

  if (budget > MAX_CHUNK)
    budget = MAX_CHUNK;

  return budget;

 sleep:
  pthread_mutex_unlock(&budget_mtx);
  usleep(1000);
  goto again;
}

/* Always sends from file offset 0. */
static ssize_t portable_sendfile(int fd_out, int fd_in, size_t size)
{
  off_t o = 0;
#ifdef __linux__
  return sendfile(fd_out, fd_in, &o, size);
#elif __FreeBSD__
  assert(size <= MAX_CHUNK);
  ssize_t res = 0;
  int ret = sendfile(fd_in, fd_out, o, size, NULL, &res, 0);
  if ((ret == -1) && (errno == EINVAL)) {
    printf("EINVAL: sendfile(%u, %u, %lu, %zu) = %d, %zd\n", fd_in, fd_out, (unsigned long)o, size, ret, res);
    return 1;
  } 
  return res;
#else
# error "Your OS is not supported. Implement sendfile."
#endif
}

static void *handler_fn(void *p)
{
  unsigned long long bytes_sent = 0;
  uint32_t budget = 0;
  int sock = (uintptr_t)p;
  ssize_t cur;

  if (shutdown(sock, receiving ? SHUT_WR : SHUT_RD) < 0) { perror("shutdown"); goto close_it; }

  int sz = 1 << 18;
  if (setsockopt(sock, SOL_SOCKET, receiving ? SO_RCVBUF : SO_SNDBUF, &sz, sizeof(sz)) < 0)
    goto close_it;

  if (receiving) {
    void *buf = malloc(MAX_CHUNK);

    while ((cur = read(sock, buf, budget ? budget : (budget = get_budget()))) > 0) {
	bytes_sent += cur;
	assert(cur <= budget);
	budget     -= cur;
    }
    free(buf);
  } else {
    while ((cur = portable_sendfile(sock, devzero, get_budget())) > 0)
      bytes_sent += cur;
  }

 close_it:
  printf("Sent %llu bytes.\n", bytes_sent);
  perror("xmit");
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


  /* Create temporary file */
  char tmpn[] = "/tmp/benchcat.XXXXXXXX";
  devzero = mkstemp(tmpn);
  if (devzero < 0)      { perror("open"  ); return EXIT_FAILURE; }
  if (unlink(tmpn) < 0) { perror("unlink"); return EXIT_FAILURE; }
  if (lseek(devzero, MAX_CHUNK - 1, SEEK_SET) < 0)
    { perror("seek"); return EXIT_FAILURE; }
  if (write(devzero, "\0", 1) != 1)
    { perror("write"); return EXIT_FAILURE; }

  if (argc != 4) {
    print_help();
    return EXIT_FAILURE;
  }
  
  bytes_per_second = strtoull(argv[1], NULL, 0) * 1000 * 1000 / 8;
  external_port    = strtoul(argv[2], NULL, 0);
  receiving        = (strcmp(argv[3], "recv") == 0);
  if (!receiving && (strcmp(argv[3], "send") != 0)) {
    fprintf(stderr, "Last parameter must be 'send' or 'recv'\n");
    return EXIT_FAILURE;
  }

  printf("%s %" PRIu64 " MBit/s on port %u.\n", receiving ? "Input" : "Output", 8*bytes_per_second / (1000 * 1000), external_port);

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
