#include <stdio.h>
#include <stdlib.h>

typedef struct node {
  char *fileName;
  struct node *next;
} Node;

typedef struct list {
  Node *head; 
} List;

Node *CreateNode(char *fileName){
  Node *newNode = malloc(sizeof(Node));
  newNode->fileName = fileName;
  newNode->next = NULL;

  return newNode;
}

List *InitList(){
  List *list = malloc(sizeof(List));
  list->head = NULL;

  return list;
}

void Append(char *fileName, List *list) {
    Node *current = NULL;
    if(list->head == NULL){
        list->head = CreateNode(fileName);
    }
    else {
        current = list->head; 
        while (current->next != NULL){
            current = current->next;
        }
    
        current->next = CreateNode(fileName);
    }
}

void Remove(char *fileName, List *list) {
    Node *current = list->head;            
    Node *previous = current;           
    while(current != NULL){           
        if(current->fileName == fileName){      
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