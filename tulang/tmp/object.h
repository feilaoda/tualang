
#define ObjectHeader	struct gc_object *next; byte tag; byte marked


typedef struct gc_object {
  ObjectHeader;
} gc_object;


typedef struct gc_string {
  ObjectHeader;
  int length;
  char *data;
} gc_string;

