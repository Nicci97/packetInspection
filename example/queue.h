/*
 * File   : queue.h
 * Author : zentut.com
 * Purpose: stack header file
 * Sourced from: http://www.zentut.com/c-tutorial/c-queue/
 */
#ifndef QUEUE_H_INCLUDED
#define QUEUE_H_INCLUDED

#include <sys/types.h>

void init(int *head, int *tail);
void enqueue(u_char **q,int *tail, u_char *element);
u_char* dequeue(u_char *q,int *head);
int empty(int head,int tail);
int full(int tail,const int size);
void display(u_char *q,int head,int tail);
#endif // QUEUE_H_INCLUDED