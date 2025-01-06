#include "tua.h"
#include "pool.h"


#define MAX_INT 2147483647
#define TUA_STACK_SIZE 512



int REG(char i) {
    int r = i-'a'+1;
    // printf("get rg:%c=%d\n", i,r);
    return r;
}


tua_state* tua_newstate(int stack_size) {
    struct tua_state* t = (tua_state*)malloc(sizeof(struct tua_state));

    NEW_STACK(t->st, stack_size);
    t->pool = pool_create(POOL_MAX_ALLOC_FROM_POOL);

    // t->st = malloc(sizeof(struct tua_stack));
    // t->st->value = malloc(sizeof(tua_stack_value)*stack_size);
    // t->st->size = stack_size;
 
    return t;
}

void tua_free(tua_state *t) {
    free(t->st->value);
    free(t->st);
    free(t);
}


int parse_int(const char *s) {
    int n = 0;
    while (*s) {
        n = n * 10 + (*s - '0');
        s++;
    }
    return n;
}



void assert_equal(int a, int b) {
    if(a != b) {
        printf("assert failed %d != %d\n", a, b);
    }else {
        printf(" true\n");
    }
}


tua_instruction parse_line(char *line, int *labels) {
    char arg1[32];
    char arg2[32];
    char arg3[32];
    char op[32];
    int argc1,argc2,argc3;

    sscanf(line, "%s %s %s %s", op, arg1, arg2, arg3);
    // printf("parse line %s %s %s %s\n", op,  arg1, arg2, arg3);
    if(arg1[0] == 'R' || arg1[0] == '$' || arg1[0] == '#') {
        argc1 = atoi(arg1+1);
    }else {
        argc1 = atoi(arg1);
    }
    
    if(arg2[0] == 'R' || arg2[0] == '$' || arg2[0] == '#') {
        argc2 = atoi(arg2+1);
    }else if(arg2[0] == '.') {
        int pos = atoi(arg2+2);
        argc2 = labels[pos];
    }else {
        argc2 = atoi(arg2);
    }
    
    if(arg3[0] == 'R' || arg3[0] == '$' || arg3[0] == '#') {
        argc3 = atoi(arg3+1);
    }else if(arg3[0] == '.') {
        int pos = atoi(arg3+2);
        argc3 = labels[pos];
    }
    else{
        argc3 = atoi(arg3);
    }
    

    tua_instruction i = 0;

    OpCode opcode = str_to_opcode(op);
    SET_OPCODE(i, opcode);
    if(opcode == OP_LOADI) {
        // printf("LOADI %d\n", argc2);
        SETARG_sBx(i, argc2);
    }else {
        SETARG_B(i, argc2);
        SETARG_C(i, argc3);
    }
    SETARG_A(i, argc1);
    
    return i;
}



int run_tua(const char * filename, int *argv, int argc) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        perror("Failed to open file");
        exit(EXIT_FAILURE);
    }
    char *lines[1024] = {0};
    char line[256];
    tua_bytecode bytecodes[256];
    int num_bytecode = 0;
    int labels[256] = {0};
    tua_instruction instructions[256] = {0};
    while (fgets(line, sizeof(line), file)) {
        if(line[0] == ';'|| (line[0] == '#') || line[0] == '\n') {
            continue;
        }
        lines[num_bytecode] = malloc(256);
        strcpy(lines[num_bytecode],line);
        if(line[0] == '.' && line[1] == 'L') {
            int pos = atoi(line+2);
            // printf("parse label %d\n", pos);
            labels[pos] = num_bytecode;
        }
        num_bytecode++;
        // printf("line: %s, num:%d\n",line,num_bytecode);
    }

    char other[256] = {0};
    char label[32] = {0};
    for(int i=0; i<num_bytecode; i++) {
        char *code = lines[i];
        // printf("code: %s",code);
        if(code[0] == '.') {
            sscanf(code, "%s %s", label, other);
            char * ptr = strchr(code, ' ');
            tua_instruction ins = parse_line(ptr + 1, labels);
            instructions[i] = ins;
        }else {
            tua_instruction ins = parse_line(code, labels);
            instructions[i] = ins;
        }
    }

    fclose(file);
    time_t s = time(NULL);
    struct timeval stop, start;
  //do stuff

    tua_state *t = tua_newstate(TUA_STACK_SIZE);
    t->savedpc = instructions;

    // print_bytecodes(bytecodes, num_bytecode);

    for (int i = 0; i < num_bytecode; i++)
    {
        /* code */
        // printf("0x%04X, %d\n", bytecodes[i].i,bytecodes[i].i);   
    }
    
    int ret;
    tua_call_info ci;
    ci.st_start = t->st->value;
    ci.st_top = ci.st_start+TUA_STACK_SIZE;
    ci.fn.t.savedpc = instructions;
    ci.prev = NULL;
    ci.next = NULL;

    // tua_call_info *next = malloc(sizeof(tua_call_info));
    // ci.next = next;
    printf("instructions: %p\n",instructions);
    ret = tua_execute(t, &ci, labels, argv);

    tua_free(t);

    return ret;
}


// int run_tua2(const char * file, int * argv, int argc) {
//     int opcodes[] = {
//         0x0091,
// 0x0194,
// 0x0111,
// 0x3020195,
// 0x3010082,
// 0x10196,
// 0x0087
//     };
//     tua_bytecode bytecodes[256];
//     int num = sizeof(opcodes)/sizeof(int);
//     tua_instruction instructions[256] = {0};

//     for(int i = 0; i < num; i++) {
//         bytecodes[i].i = opcodes[i];
//         instructions[i] = opcodes[i];
//     }
//     for (int i = 0; i < num; i++)
//     {
//         /* code */
//         // printf("0x%04X, %d\n", bytecodes[i].i,bytecodes[i].i);   
//     }
//     tua_state *t = tua_newstate(TUA_STACK_SIZE);
//     t->savedpc = instructions;
//     int labels[256] = {0};
//     int ret = tua_execute(t, labels, argv);
//     tua_free(t);
//     return ret;

// }

struct timeval stop, start;

void test0(int argc, const char *argv[]) {
    int *v = malloc(sizeof(int)*(argc-2));
    for(int i=2;i<argc;i++) {
        v[i-2] = atoi(argv[i]);
        // printf("argv: %d=%d\n", i-2, v[i-2]);
    }
    // int v = atoi(argv[2]);
    gettimeofday(&start, NULL);
    assert_equal(0, run_tua(argv[1], v, argc-2));
    gettimeofday(&stop, NULL);
    printf("====result0: time: %fs\n",(float)((stop.tv_sec - start.tv_sec) * 1000000 + stop.tv_usec - start.tv_usec)/1000000.0);
}

int main(int argc, const char *argv[]) {
    if(argc < 3) {
        printf("Usage: tua <file> <number1>\n");
        return 1;
    }
    // assert_equal(8, run_tua("test1.tvm", (int[]){v}, 1));
    
    test0(argc, argv);
    
    return 0;
}

