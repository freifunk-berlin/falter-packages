#include "./libhna/hna.h"
#include "libavl/avl.h"
#include "libhna/avl_helper.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
        /*const char *hna4 = tcp_read("127.0.0.1", 2006, "/hna");
        const char *hna6 = tcp_read("::1", 2006, "/hna");

        if (hna4 == NULL) {
          fprintf(stderr, "failed to read ipv4 HNAs\n");
          return EXIT_FAILURE;
        }

        //if (hna6 == NULL) {
        //  fprintf(stderr, "failed to read ipv6 HNAs\n");
        //  return EXIT_FAILURE;
        //}*/

        char *hna4 = read_file("raw/hna4_2006.txt");
        char *hna6 = read_file("raw/hna6_2006.txt");
        char *hosts = read_file("raw/olsr");
        // const char *hosts = read_file("/tmp/hosts/olsr");

        // read data into AVL-Treee
        struct avl_traverser traverser;
        struct avl_table *tree = avl_create(&compare_node_structs, NULL, NULL);

        read_hna_into_tree(tree, hna4);
        read_hosts_into_tree(tree, hosts);

        // tree: inorder-walk and print data
        char* hna = "Announced network";
        char* gw = "OLSR gateway";
        char* v_time = "Validity Time";
        char* h_name = "OLSR Hostname";
        printf("\n%-17s\t%-15s\t%-14s\t%-17s\n", hna, gw, v_time,h_name);
        printf("=================\t============\t=============\t=============\n");
        hna_data *curr;
        curr = (hna_data *) avl_t_first(&traverser, tree);
        do {
                char hna_addr[IP_ADDR_MAX_STR_LEN];
                inet_ntop(AF_INET, &curr->hna.base_addr.sin_addr, hna_addr, IP_ADDR_MAX_STR_LEN);
                strncat(hna_addr,"/", 2);
                strncat(hna_addr,curr->hna.netmask, 3);

                char gw_addr[IP_ADDR_MAX_STR_LEN];
                inet_ntop(AF_INET, &curr->via_gateway.sin_addr, gw_addr, IP_ADDR_MAX_STR_LEN);

                if (curr->host_name == NULL) {
                        curr->host_name = "(nil)";
                }

                printf("%-18s\t%-15s\t%8d%s\t%-17s\n", hna_addr, gw_addr, 0, "    ", curr->host_name);
                curr = (hna_data *) avl_t_next(&traverser);
        } while (curr != NULL);

        // free avl-tree
        avl_destroy(tree, &free_hna_data);

        free(hna4);
        free(hosts);
        free(hna6);

        return EXIT_SUCCESS;
}
