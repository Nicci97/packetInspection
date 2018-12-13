/*
 * File   : queue.c
 * Author : zentut.com
 * Purpose: stack header file
 * Sourced from: http://www.zentut.com/c-tutorial/c-queue/
 */
/*
    initialize queue pointers
*/
#include <stdio.h>
#include <sys/types.h>

void init(int *head, int *tail)
{
    *head = *tail = 0;
}

/*
   enqueue an element
   precondition: the queue is not full
*/
void enqueue(u_char **q ,int *tail, u_char *element)
{
    q[(*tail)++] = element;
}

/*
    dequeue an element
    precondition: queue is not empty
*/
u_char* dequeue(u_char **q,int *head)
{
    return q[(*head)++];
}

/*
    return 1 if queue is full, otherwise return 0
*/
int full(int tail,const int size)
{
    return tail == size;
}

/*
  return 1 if the queue is empty, otherwise return 0
*/
int empty(int head, int tail)
{
    return tail == head;
}

/*
  display queue content
*/
void display(u_char *q,int head,int tail)
{
    int i = tail - 1;
    while(i >= head)
    {
        printf("%d ",q[i--]);
    }
    printf("\n");
}