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

void * listPop(List* list) {
    if (!list || list->length == 0) return NULL;
    if (list->length == 1) {
        void* data = list->head->data;
        free(list->head);
        list->head = list->tail = NULL;
        list->length = 0;
        return data;
    }

    ListNode* prev = list->head;
    while (prev->next && prev->next != list->tail) {
        prev = prev->next;
    }
    void* data = list->tail->data;
    free(list->tail);
    prev->next = NULL;
    list->tail = prev;
    list->length--;
    return data;
}
