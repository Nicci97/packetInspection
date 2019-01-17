/* Sourced from https://www.techiedelight.com/queue-implementation-using-linked-list/ and altered*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "queue.h"
#include "bits/types/struct_timeval.h"
#include "pcap.h"

// Utility function to allocate the new queue node
struct Node* newNode(u_char* item, struct pcap_pkthdr *h, u_int8_t* protocol_hash, u_int16_t* vlan_id_hash, u_int32_t* src_ip_hash, 
            u_int32_t* dst_ip_hash, u_int16_t* src_port_hash, u_int16_t* dst_port_hash)
{
	// Allocate the new node in the heap
	struct Node* node = (struct Node*)malloc(sizeof(struct Node));

	// check if queue (heap) is full. Then inserting an element would
	// lead to heap overflow
	if (node != NULL)
	{
		// set the data in allocated node and return the node
		node->data = item;
		node->header = h;
		node->next = NULL;
		node->protocol = *protocol_hash;
		node->vlan_id = *vlan_id_hash;
		node->src_ip = *src_ip_hash;
		node->dst_ip = *dst_ip_hash;
		node->src_port = *src_port_hash;
		node->dst_port = *dst_port_hash;
		return node;
	}
	else
	{
		printf("\nHeap Overflow");
		exit(EXIT_FAILURE);
	}
}

// Utility function to remove front element from the queue
struct Node* dequeue(struct Node **front, struct Node **rear) // delete at the beginning
{
	if (*front == NULL) 
	{
		printf("\nQueue Underflow");
		exit(EXIT_FAILURE);
	}

	struct Node *temp = *front;
	//printf("Removing %s\n", temp->data);

	// advance front to the next node
	*front = (*front)->next;

	// if list becomes empty
	if (*front == NULL) {
		*rear = NULL;
	}

	// deallocate the memory of removed node and 
	// optionally return the removed item
	u_char* item = temp->data;
	//free(temp);
	//printf("Dequeue %s\n", item);
	return temp;
}

// Utility function to add an item in the queue
void enqueue(u_char* i, struct pcap_pkthdr **h, struct Node **front, struct Node **rear, u_int8_t* protocol_hash, u_int16_t* vlan_id_hash, u_int32_t* src_ip_hash, 
            u_int32_t* dst_ip_hash, u_int16_t* src_port_hash, u_int16_t* dst_port_hash) // insertion at the end
{
	// Create a deep copy of the header in memory
	// copy header struct
	struct pcap_pkthdr *header = *h;	
	struct pcap_pkthdr *headerCopy;
	headerCopy = malloc(sizeof(struct pcap_pkthdr));
	memcpy(headerCopy, header, sizeof(struct pcap_pkthdr));
	
	// copy packet data
	u_char* item = i;
	u_char* itemCopy;
	itemCopy = malloc(header->caplen);
	memcpy(itemCopy, item, header->caplen);

	
	// create new node to put in linked list
	struct Node* node = newNode(itemCopy, headerCopy, protocol_hash, vlan_id_hash, src_ip_hash, dst_ip_hash, 
           src_port_hash, dst_port_hash);

	// special case: queue was empty
	if (*front == NULL) 
	{
		// initialize both front and rear
		*front = node;
		*rear = node;
	}
	else
	{
		// update rear
		(*rear)->next = node;
		*rear = node;
	}
}

// Utility function to return top element in a queue
u_char* peek(struct Node *front)
{
	// check for empty queue
	if (front != NULL)
		return front->data;
	else
		exit(EXIT_FAILURE);
}

// Utility function to check if the queue is empty or not
int isEmpty(struct Node **front, struct Node **rear)
{
	return *rear == NULL && *front == NULL;
}

// main function
// int main()
// {
// 	enqueue(1);
// 	enqueue(2);
// 	enqueue(3);
// 	enqueue(4);
 
// 	printf("Front element is %d\n", peek());

// 	dequeue();
// 	dequeue();
// 	dequeue();
// 	dequeue();
 
// 	if (isEmpty())
// 		printf("Queue is empty");
// 	else
// 		printf("Queue is not empty");

// 	return 0;
// }