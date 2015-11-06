#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "../include/uthash.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#define NOTGIOS_MONITOR_PORT 31089
#define NOTGIOS_STATIC_BUFSIZE 256
#define NOTGIOS_SUCCESS 0x0
#define NOTGIOS_GENERIC_ERROR 0x01
#define NOTGIOS_BAD_HOSTNAME 0x02
#define NOTGIOS_SERVER_UNREACHABLE 0x04
#define NOTGIOS_SERVER_REJECTED 0x08

int handshake(char *server_hostname, int port);
void launch_worker_thread(void *args);
void user_error();

int main(int argc, char **argv) {
  int c, port = 0;
  char *server_hostname = NULL;

  opterr = 0;
  switch ((c = getopt(argc, argv, "s:p:")) != -1) {
    case 's':
      server_hostname = optarg;
      break;
    case 'p':
      port = atoi(optarg);
      break;
    default:
      user_error();
      return EXIT_FAILURE;
  }
  if (!server_hostname || !port) {
    user_error();
    return EXIT_FAILURE;
  }

  switch (handshake(server_hostname, port)) {
    case 0:
      break;
    case NOTGIOS_SERVER_REJECTED:
    case NOTGIOS_BAD_HOSTNAME:
      user_error();
    default:
      return EXIT_FAILURE;
  }

  // We've finished the handshake, and must prepare to work.

}

int handshake(char *server_hostname, int port) {
  int sockfd, actual = 0, sleep_period = 1, expected;
  struct sockaddr_in serv_addr;
  struct hostent *server;
  char buffer[NOTGIOS_STATIC_BUFSIZE];

  // Begin long and arduous connection process...
  memset(buffer, 0, sizeof(char) * NOTGIOS_STATIC_BUFSIZE);
  sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) return NOTGIOS_GENERIC_ERROR;
  server = gethostbyname(server_hostname);
  if (!server) return NOTGIOS_BAD_HOSTNAME;
  memset(&serv_addr, 0, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);
  serv_addr.sin_port = htons(port);
  while (connect(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
    sleep(sleep_period);
    sleep_period *= 2;
    if (sleep_period == 32) return NOTGIOS_SERVER_UNREACHABLE;
  }

  // Send hello message to server.
  sprintf(buffer, "NGS HELLO\nCMD PORT %d\n\n", NOTGIOS_MONITOR_PORT);
  expected = strlen(buffer) + 1;
  while (actual != expected) write(sockfd, buffer, strlen(buffer) + 1);

  // Get server's response.
  actual = 0;
  memset(buffer, 0, sizeof(char) * NOTGIOS_STATIC_BUFSIZE);
  while (buffer[strlen(buffer) - 1] != '\n' || buffer[strlen(buffer) - 2] != '\n') {
    actual += read(sockfd, buffer, NOTGIOS_STATIC_BUFSIZE);
  }
  if (strstr(buffer, "NGS ACK") == buffer) {
    close(sockfd);
    return NOTGIOS_SUCCESS;
  } else if (strstr(buffer, "NGS NACK") == buffer) {
    close(sockfd);
    return NOTGIOS_SERVER_REJECTED;
  } else {
    close(sockfd);
    return NOTGIOS_GENERIC_ERROR;
  }
}
