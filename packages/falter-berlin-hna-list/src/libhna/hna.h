#ifndef CODE_HNA_H
#define CODE_HNA_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "../libavl/avl.h"

#define IP_ADDR_MAX_STR_LEN 46 // iclusive \0-byte

typedef struct {
        struct sockaddr_in base_addr;
        // two chars plus \0-byte
        char* netmask;
} cidr_address;

typedef struct {
        struct sockaddr_in via_gateway;
        cidr_address hna;
        uint8_t valid_time;
        char* host_name;
} hna_data;

char* read_file(const char *filename);

int find_host(char *name, const char *addr, const size_t addr_len, const char *hosts, const size_t hosts_len);

char *tcp_read(const char *addr_str, int port, const char *request);

// 10.230.198.192/28	10.31.43.188
hna_data* serialize_hna_string(char* hna, char* gateway);

void read_hna_into_tree(struct avl_table *tree, char *raw_data);

void read_hosts_into_tree(struct avl_table *tree, char *raw_data);


#endif //CODE_HNA_H
