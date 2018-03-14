#include <stdio.h>
#include <stdlib.h>

typedef struct node {
  char *filename;
  struct node *next;
} Node;

typedef struct list {
  Node *head; 
} List;

Node *CreateNode(char *filename){
  Node *newNode = malloc(sizeof(Node));
  newNode->filename = filename;
  newNode->next = NULL;

  return newNode;
}

List *InitList(){
  List *list = malloc(sizeof(List));
  list->head = NULL;

  return list;
}

void Append(char *filename, List *list) {
    Node *current = NULL;
    if(list->head == NULL){
        list->head = CreateNode(filename);
    }
    else {
        current = list->head; 
        while (current->next != NULL){
            current = current->next;
        }
    
        current->next = CreateNode(filename);
    }
}

void Remove(char *filename, List *list) {
    Node *current = list->head;            
    Node *previous = current;           
    while(current != NULL){           
        if(current->filename == filename){      
            previous->next = current->next;
            if(current == list->head) {
                list->head = current->next;
            }

            free(current);
            return;
        }

        previous = current;             
        current = current->next;        
    }                                 
}

void RemoveAt(Node *node, List *list) {
    Node *current = node;            
    Node *previous = current;

    previous->next = current->next;
    if(current == list->head) {
        list->head = current->next;
    }

    free(current);
    return;
}

int Contains(List *list, char *name) {
    Node *current = list->head;
    while(current != NULL){           
        if(strcmp(current->filename, name) == 0){
            return 1;
        }

        current = current->next;        
    }        

    return -1;         
}

void DestroyList(List *list) {
    Node *current = list->head;
    Node *tmp;
    while(current != NULL) {
        tmp = current->next;
        // RemoveAt(current, list);
        Remove(current->filename, list);
        current = tmp;
    }    
}