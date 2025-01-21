#include <stdlib.h>
#include "list.h"



List* listNew() {
    List* list = malloc(sizeof(List));
    list->head = list->tail = NULL;
    list->length = 0;
    return list;
}

List* listAppend(List* list, void* data) {
    ListNode* node = malloc(sizeof(ListNode));
    node->data = data;
    node->next = NULL;
    
    if (list->tail == NULL) {
        list->head = list->tail = node;
    } else {
        list->tail->next = node;
        list->tail = node;
    }
    list->length++;
    return list;
}


void listFree(List* list) {
    ListNode* node = list->head;
    while (node != NULL) {
        ListNode* next = node->next;
        free(node);
        node = next;
    }
    free(list);
}

void * listGet(List* list, int index) {
    ListNode* node = list->head;
    for (int i = 0; i < index; i++) {
        node = node->next;
    }
    return node->data;
}

