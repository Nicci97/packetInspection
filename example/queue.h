/*
 * File   : queue.h
 * Author : zentut.com
 * Purpose: stack header file
 * Sourced from: http://www.zentut.com/c-tutorial/c-queue/
 */
// #ifndef QUEUE_H_INCLUDED
// #define QUEUE_H_INCLUDED
 #include <sys/types.h>

// A linked list node
struct Node
{
	u_char* data;				// integer data
	struct pcap_pkthdr* header; // header for current packet
	struct Node* next;		// pointer to the next node
	u_int8_t protocol;
    u_int16_t vlan_id;
    u_int32_t src_ip;
    u_int32_t dst_ip;
    u_int16_t src_port;
    u_int16_t dst_port;
};

struct Node* newNode(u_char* item, struct pcap_pkthdr *h, u_int8_t* protocol_hash, u_int16_t* vlan_id_hash, u_int32_t* src_ip_hash, 
            u_int32_t* dst_ip_hash, u_int16_t* src_port_hash, u_int16_t* dst_port_hash);
struct Node* dequeue(struct Node **front, struct Node **rear);
void enqueue(u_char* item, struct pcap_pkthdr **header, struct Node **front, struct Node **rear, u_int8_t* protocol_hash, u_int16_t* vlan_id_hash, u_int32_t* src_ip_hash, 
            u_int32_t* dst_ip_hash, u_int16_t* src_port_hash, u_int16_t* dst_port_hash);
u_char* peek(struct Node *front);
int isEmpty(struct Node **front, struct Node **rear);
// #endif // QUEUE_H_INCLUDED

