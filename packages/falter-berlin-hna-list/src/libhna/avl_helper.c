#include "avl_helper.h"
#include "../libavl/avl.h"
#include "hna.h"
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>


//int compare_dataset(const void *avl_a, const void *avl_b, void *avl_param) {
int compare_node_structs(const void *avl_a, const void *avl_b, void *avl_param) {
        hna_data *a = (hna_data *) avl_a;
        hna_data *b = (hna_data *) avl_b;

        struct sockaddr_in sa = a->via_gateway;
        struct sockaddr_in sb = b->via_gateway;

        struct sockaddr_in *a_addr = (struct sockaddr_in *) &sa;
        struct sockaddr_in *b_addr = (struct sockaddr_in *) &sb;

        if (a_addr->sin_addr.s_addr < b_addr->sin_addr.s_addr)
                return -1;
        else if (a_addr->sin_addr.s_addr > b_addr->sin_addr.s_addr)
                return 1;
        else
                return 0;
}

//avl_item_func (void *avl_item, void *avl_param);
void free_hna_data(void *avl_item, void *avl_param) {
        hna_data *item = (hna_data*)avl_item;
        //free(item->host_name);
        //free(item->hna.netmask);
        free(item);
}
