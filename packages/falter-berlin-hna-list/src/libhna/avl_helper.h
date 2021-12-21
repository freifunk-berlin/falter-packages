#ifndef CODE_HELPER_H
#define CODE_HELPER_H

/**
 * Compares two structs hna_data for insertion into AVL-Tree.
 * Oders by natural ordering of the binary form of gw_ip_addresses.
 *
 * @param avl_a
 * @param avl_b
 * @param avl_param
 * @return
 */
int compare_node_structs(const void *avl_a, const void *avl_b, void *avl_param);

void free_hna_data(void *avl_item, void *avl_param);

#endif
