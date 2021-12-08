#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "libavl/avl.h"

static char* read_file(const char *filename)
{
  char *buffer;
  FILE *f = fopen(filename, "rb");

  if (!f) {
    fprintf(stderr, "failed to open %s\n", filename);
    return NULL;
  }
  
  fseek(f, 0, SEEK_END);
  long length = ftell (f);
  fseek(f, 0, SEEK_SET);
  buffer = malloc(length);
  if (buffer) {
    fread(buffer, 1, length, f);
  }
  fclose(f);

  return buffer;
}

static int find_host(char *name, const char *addr, const size_t addr_len, const char *hosts, const size_t hosts_len)
{
  const char *beg = hosts;
  const char *end = hosts + hosts_len;
  const char *p = hosts; 
  while (p < end) {
    p = strstr(p + 1, addr);
    if (p == NULL) {
      return 0;
    }
    if ((p == beg || *(p-1) == '\n') && (p == end || *(p+addr_len) == '\t')) {
      const char *tok_beg = p + addr_len + 1;
      const char *tok_end = tok_beg;
      while (tok_end < end) {
        if (*tok_end == '\t' || *tok_end == '\n' || *tok_end == ' ') {
          break;
        }
        tok_end += 1;
      }
      memcpy(name, tok_beg, tok_end - tok_beg);
      name[(tok_end - tok_beg)] = '\0';
      //printf("found: '%.*s'\n", (int) (tok_end - tok_beg), tok_beg);
      return 1;
    }
  }
  return 0;
}

static char *tcp_read(const char *addr_str, int port, const char *request)
{
    struct sockaddr_storage server_addr;

    size_t buffer_len = 4096;
    char *buffer = malloc(buffer_len);
    int sock = socket(AF_INET, SOCK_STREAM, 0);

    if (sock < 0) {
        printf("socket(): %s\n", strerror(errno));
        return NULL;
    }
 
    struct sockaddr_in *addr4 = (struct sockaddr_in *)&server_addr;
    struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&server_addr;

    if (inet_pton(AF_INET, addr_str, &addr4->sin_addr) == 1) {
      addr4->sin_family = AF_INET;
      addr4->sin_port = htons(port);
    } else if (inet_pton(AF_INET6, addr_str, &addr6->sin6_addr) == 1) {
      addr6->sin6_family = AF_INET6;
      addr6->sin6_port = htons(port);
    } else {
        printf("invalid address: %s\n", addr_str);
        return NULL;
    }
   
    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        printf("connect(): %s\n", strerror(errno));
        return NULL;
    }

    send(sock, request , strlen(request), 0);
    int valread = read(sock, buffer, buffer_len - 1);
    if (valread > 0) {
      return buffer;
    } else {
      return NULL;
    }
}

int main(int argc, char *argv[])
{
  const char *hna4 = tcp_read("127.0.0.1", 2006, "/hna");
  const char *hna6 = tcp_read("::1", 2006, "/hna");

  if (hna4 == NULL) {
    fprintf(stderr, "failed to read ipv4 HNAs\n");
    return EXIT_FAILURE;
  }

  if (hna6 == NULL) {
    fprintf(stderr, "failed to read ipv6 HNAs\n");
    return EXIT_FAILURE;
  }

  //const char *hna4 = read_file("hna4.txt");
  //const char *hna6 = read_file("hna6.txt");
  const char *hosts = read_file("/tmp/hosts/olsr");

  int dst_start = 0;
  int gw_start = 0;
  for (int i = 0; i < strlen(hna4); i++) {
      const char c = hna4[i];
      if (c == '\n') {
        if (dst_start < gw_start) {
          const char *dst = &hna4[dst_start];
          const int dst_len = gw_start - dst_start - 1;
          const char *gw = &hna4[gw_start];
          const int gw_len = i - gw_start - 1;

          char gw_name[64] = {0};
          int rc = hosts ? find_host(gw_name, gw, gw_len, hosts, strlen(hosts)) : 0;
          printf("%-25.*s %-20.*s %s\n", dst_len, dst, gw_len, gw, rc ? gw_name : "");
        }
        dst_start = i + 1;
      } else if (c == '\t') {
        gw_start = i + 1;
      }
  }

  return EXIT_SUCCESS;
}
