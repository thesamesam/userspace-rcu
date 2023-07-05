// SPDX-FileCopyrightText: 2013 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: MIT

/*
 * This example shows how to pop nodes from a lfstack.
 */

#include <stdio.h>
#include <stdlib.h>

#include <urcu/lfstack.h>	/* Wait-free stack */
#include <urcu/compiler.h>	/* For CAA_ARRAY_SIZE */

/*
 * Nodes populated into the stack.
 */
struct mynode {
	int value;			/* Node content */
	struct cds_lfs_node node;	/* Chaining in stack */
};

int main(void)
{
	int values[] = { -5, 42, 36, 24, };
	struct cds_lfs_stack mystack;	/* Stack */
	unsigned int i;
	int ret = 0;

	cds_lfs_init(&mystack);

	/*
	 * Push nodes.
	 */
	for (i = 0; i < CAA_ARRAY_SIZE(values); i++) {
		struct mynode *node;

		node = malloc(sizeof(*node));
		if (!node) {
			ret = -1;
			goto end;
		}

		cds_lfs_node_init(&node->node);
		node->value = values[i];
		cds_lfs_push(&mystack, &node->node);
	}

	/*
	 * Pop nodes from the stack, one by one, from newest to oldest.
	 */
	printf("pop each mystack node:");
	for (;;) {
		struct cds_lfs_node *snode;
		struct mynode *node;

		snode = cds_lfs_pop_blocking(&mystack);
		if (!snode) {
			break;
		}
		node = caa_container_of(snode, struct mynode, node);
		printf(" %d", node->value);
		free(node);
	}
	printf("\n");
end:
	cds_lfs_destroy(&mystack);
	return ret;
}
