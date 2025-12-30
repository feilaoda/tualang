#ifndef _LIST_H_
#define _LIST_H_


typedef struct ListNode {
    void* data;
    struct ListNode* next;
} ListNode;

typedef struct List {
    ListNode* head;
    ListNode* tail;
    int length;
} List;

// List operations
List* listNew(void);
List* listAppend(List* list, void* data);
void listFree(List* list);
void * listGet(List* list, int index);
void * listPop(List* list);

#endif
