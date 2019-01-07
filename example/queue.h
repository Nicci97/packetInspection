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
	struct Node* next;		// pointer to the next node
};

struct Node* newNode(u_char* item);
u_char* dequeue(struct Node **front, struct Node **rear);
void enqueue(u_char* item, struct Node **front, struct Node **rear);
u_char* peek(struct Node *front);
int isEmpty(struct Node **front, struct Node **rear);
// #endif // QUEUE_H_INCLUDED