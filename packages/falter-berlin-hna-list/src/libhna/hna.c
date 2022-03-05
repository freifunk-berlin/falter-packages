#include "hna.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

char *read_file(const char *filename) {
        char *buffer;
        FILE *f = fopen(filename, "rb");

        if (!f) {
                fprintf(stderr, "failed to open %s\n", filename);
                return NULL;
        }

        fseek(f, 0, SEEK_END);
        long length = ftell(f);
        fseek(f, 0, SEEK_SET);
        buffer = calloc(1, length + 1);
        if (buffer) {
                fread(buffer, 1, length, f);
        }
        fclose(f);

        return buffer;
}

int open_file(const char *filename) {
        FILE *f = fopen(filename, "rb");

        if (!f) {
                fprintf(stderr, "failed to open %s\n", filename);
                return -1;
        }

        return f->_fileno;
}

int make_request(const char *addr_str, int port, const char *request) {
        struct sockaddr_storage server_addr;

        int sock = socket(AF_INET, SOCK_STREAM, 0);

        if (sock < 0) {
                printf("socket(): %s\n", strerror(errno));
                return -1;
        }

        struct sockaddr_in *addr4 = (struct sockaddr_in *) &server_addr;
        struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *) &server_addr;

        if (inet_pton(AF_INET, addr_str, &addr4->sin_addr) == 1) {
                addr4->sin_family = AF_INET;
                addr4->sin_port = htons(port);
        } else if (inet_pton(AF_INET6, addr_str, &addr6->sin6_addr) == 1) {
                addr6->sin6_family = AF_INET6;
                addr6->sin6_port = htons(port);
        } else {
                printf("invalid address: %s\n", addr_str);
                return -1;
        }

        if (connect(sock, (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0) {
                printf("connect(): %s\n", strerror(errno));
                return -1;
        }

        if (send(sock, request, strlen(request), 0) != -1) {
                return sock;
        } else {
                printf("send(): %s\n", strerror(errno));
                return -1;
        }

}

char *tcp_read(const char *addr_str, int port, const char *request) {
        struct sockaddr_storage server_addr;

        size_t buffer_len = 4096 * 4;
        char *buffer = malloc(buffer_len);
        int sock = socket(AF_INET, SOCK_STREAM, 0);

        if (sock < 0) {
                printf("socket(): %s\n", strerror(errno));
                return NULL;
        }

        struct sockaddr_in *addr4 = (struct sockaddr_in *) &server_addr;
        struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *) &server_addr;

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

        if (connect(sock, (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0) {
                printf("connect(): %s\n", strerror(errno));
                return NULL;
        }

        send(sock, request, strlen(request), 0);
        int valread = read(sock, buffer, buffer_len - 1);
        if (valread > 0) {
                return buffer;
        } else {
                return NULL;
        }
}


/*hna_data *serialize_hna_string(char *hna, char *gateway) {
        // input-data: "10.230.198.192/28", "10.31.43.188"
        hna_data* dataset = calloc(1, sizeof(hna_data));

        char* ipaddr = strtok(hna, "/");
        char* netmask = strtok(NULL, "/");

        //ToDo: struct sockaddr_in* addr_struct = (struct sockaddr_in)dataset->hna.base_addr;
        inet_pton(AF_INET, ipaddr, &dataset->hna.base_addr.sin_addr);
        dataset->hna.netmask = netmask;

        inet_pton(AF_INET, gateway, &dataset->via_gateway.sin_addr);

        return dataset;
}*/

void paste_data(hna_data * dataset, char *hna, char *gateway) {
        char* ipaddr = strtok(hna, "/");
        char* netmask = strtok(NULL, "/");
        
        dataset->hna.netmask = netmask;

        // transform char-representation to binary-addr and store in struct
        inet_pton(AF_INET, ipaddr, &dataset->hna.base_addr.sin_addr);
        inet_pton(AF_INET, gateway, &dataset->via_gateway.sin_addr);

        //free(ipaddr);
}

#define BUFFSIZE 4096

void read_hna_to_tree(struct avl_table *tree, int socket) {
        char* buff = calloc(1, BUFFSIZE);
        char* buff_pos = buff;

        while (read(socket, buff_pos, 1) > 0) {
                // once we reached line end, begin processing the line
                if ( *buff_pos == '\n' ) {
                        // make sure to end string with \0-byte
                        buff_pos ++;
                        *buff_pos = '\0';

                        // omit lines with table-headers...
                        if (*buff == 'T' || *buff == 'D') {
                                // skip line and reset buffer
                                buff_pos = buff;
                                printf("here...\n");
                                continue;
                        }

                        char* destination = malloc(IP_ADDR_MAX_STR_LEN);
                        char* gateway = malloc(IP_ADDR_MAX_STR_LEN);

                        if (sscanf(buff, "%s\t%s\n", destination, gateway) < 2)
                                printf("Error while processing line with scanf...\n");

                        hna_data* set = malloc(sizeof(hna_data));
                        paste_data(set, destination, gateway);

                        avl_insert(tree, set);
                }
                
                buff_pos ++;
        }
        free(buff);
}

void read_hna_into_tree(struct avl_table *tree, char *raw_data) {
        char *str1, *str2, *token, *subtoken;
        char *saveptr1, *saveptr2;
        char *line_delim = "\n";
        char *field_delim = "\t";
        int j;
        for (str1 = raw_data;;str1 = NULL) {
                token = strtok_r(str1, line_delim, &saveptr1);
                if (token == NULL) {
                        break;
                }
                // skip the table-headers
                if (token[0] == 'T' || token[0] == 'D') {
                        continue;
                }

                char* args[2] = {NULL, NULL};
                for (j=0,str2 = token;;str2 = NULL, j++) {
                        subtoken = strtok_r(str2, field_delim, &saveptr2);
                        if (subtoken == NULL)
                                break;
                        args[j] = subtoken;
                }

                hna_data* set = calloc(1, sizeof(hna_data));
                paste_data(set, args[0], args[1]);
                //free(args[0]);
                //free(args[1]);

                avl_insert(tree, set);
        }
}

void read_hosts_into_tree(struct avl_table *tree, char *raw_data) {
        char *str1, *str2, *token, *subtoken;
        char *saveptr1, *saveptr2;
        char *line_delim = "\n";
        char *field_delim = "\t";
        int j;
        for (str1 = raw_data;;str1 = NULL) {
                token = strtok_r(str1, line_delim, &saveptr1);
                if (token == NULL)
                        break;
                if (token[0] == '#')
                        continue;

                char* args[2];
                for (j=0,str2 = token;;str2 = NULL, j++) {
                        subtoken = strtok_r(str2, field_delim, &saveptr2);
                        if (subtoken == NULL)
                                break;
                        args[j] = subtoken;
                }

                // search-item needs to hold GW-ip-address in binary-form only
                hna_data search = {0};
                inet_pton(AF_INET, args[0], &search.via_gateway.sin_addr);
                hna_data *item = avl_find(tree, &search);

                if (item == NULL)
                        continue;
                else {
                        item->host_name = args[1];
                }
        }
}
