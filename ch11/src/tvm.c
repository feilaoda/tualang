#include "tvm.h"
#include "opcode.h"
#include "debug.h"

int tua_execute(tua_state *t, tua_call_info *savedci, int labels[], int *argv) {
    printf("tua_execute\n");
    int cnt = 0;
    tua_stack base_stack;
    tua_stack *st = t->st;
    tua_stack_id base;
    tua_call_info tmp_call;

    int funcs[] = {0};
    tua_call_info *ci = savedci;

    tua_instruction *savedpc = ci->fn.t.savedpc;
    
    void *env = {0};
    tua_instruction *pc;
    startfunc:
        // printf("startfunc\n");
    returning:
        // printf("returning\n");
    pc = ci->fn.t.savedpc;
    base = ci->st_start;
    
    while (1) {
        tua_instruction i = *pc;
        debug("CNT:%d i:Ox%X op:%s arg1:%d arg2:%d pc:%d\n",cnt++,i, opcode_tostr(GET_OPCODE(i)), GETARG_A(i), GETARG_B(i), *pc);
        pc++;
        cnt++;
        switch (GET_OPCODE(i))
        {
            case OP_LOADI:
            {
                // int* ra = RA(i);
                // int v = GETARG_B(i);
                // *ra = GETARG_B(i);
                RAV(i) = GETARG_sBx(i);
                
                debug("OP_LOADI R%d=%d\n", GETARG_A(i), GETARG_sBx(i));
                break;
            }
            case OP_LOADA:
            {
                // int *ra = RA(i);
                // int k = GETARG_B(i);
                // int v = argv[k];
                // *ra = v;
                RAV(i) = argv[GETARG_B(i)];
                
                debug("OP_LOADA %c/%d=%d ,%d\n", RGSTR(GETARG_A(i)),GETARG_A(i), RAV(i), GETARG_B(i));
                break;
            }
        case OP_MOV:
            /* code */
            {
                // printf("call before op:%s R%c=%d v=%d\n", opcode_tostr(instr.opcode), RGSTR(r),tua_toint(t,r), v);
                // tua_setnumber(t, bytecode.arg1, bytecode.arg2);
                // printf("call after op:%s R%c=%d v=%d\n", opcode_tostr(instr.opcode),  RGSTR(r),tua_toint(t,r), v);
                // 
                RAV(i) = RBV(i);
                debug("OP_MOV R%d=%d\n", GETARG_A(i),RBV(i));
                break;
            }
        case OP_RETURN:
        {
            // int *ra = RA(i);
            // int v = *ra;

            int v = RAV(i);

            // printf("OP_RET %c=%d cnt:%d\n", RGSTR(GETARG_A(i)),v, cnt);
            
            return v;
        }
        case OP_RETURN0:
        {
            if(ci->prev != NULL) {
                printf("RETURN0\n");
                tua_call_info *prevci = ci->prev;
                ci = prevci;
                t->ci = prevci;
                break;
            }else {
                printf("RET cnt=%d\n", cnt);
                return 0;

            }
        }
        case OP_RETURN1:
        {
            debug("RETURN1 , v=%d, nres=%d, prev=%d, prev->prev=%d\n",RAV(i), ci->nresults, ci->prev==NULL?0:1, ci->prev->prev==NULL?0:1);

            if(ci->prev != NULL) {
                if(ci->nresults == 0) {

                }else {
                    tua_stack_id from = base - 1;
                    from->v.i = RAV(i);
                }
                tua_call_info *prevci = ci->prev;
                ci = prevci;
                goto returning;
            }else {
                printf("RET cnt=%d\n", cnt);
                return 0;
            }
        }
        case OP_ADD:
        {
            RAV(i) = RBV(i) + RCV(i);
            
            debug("OP_ADD R%d=R%d+R%d %d=%d + %d, step:%d\n", GETARG_A(i), GETARG_B(i), GETARG_C(i), RAV(i), RBV(i), RCV(i), *pc);
            break;
        }
        case OP_SUB:
        {
            RAV(i) = RBV(i) - RCV(i);
            debug("OP_SUB R%d=R%d-R%d %d=%d - %d, step:%d\n", GETARG_A(i), GETARG_B(i), GETARG_C(i), RAV(i), RBV(i), RCV(i), *pc);
            break;
        }
        case OP_MUL:
        {
            RAV(i) = RBV(i) * RCV(i);
            
            // printf("OP_MUL %c=%c+%c %d,%d,%d, step:%d\n", RGSTR(GETARG_A(i)), RGSTR(GETARG_B(i)), RGSTR(GETARG_C(i)), *ra, *rb, *rc, *pc);
            break;
        }
        case OP_DIV:
            RAV(i) = RBV(i) / RCV(i);
            
            break;
        case OP_MOD:
        {
            RAV(i) = RBV(i) % RCV(i);
            
            break;
        }
        case OP_ADDI:
        case OP_SUBI:
        {
            RAV(i) = RBV(i) - GETARG_C(i);
            debug("OP_SUBI R%d=R%d-R%d %d=%d - %d, step:%d\n", GETARG_A(i), GETARG_B(i), GETARG_C(i), RAV(i), RBV(i), GETARG_C(i), *pc);
            break;
        }
        case OP_MULI:
        case OP_DIVI:
        case OP_MODI:
        {

            // printf("call before op:%s R%c=%d R%c=%d R%c=%d\n", opcode_tostr(instr.opcode), pos1+'a'-1,tua_toint(t,pos1), pos2+'a'-1,tua_toint(t,pos2), r3==1? pos3+'a'-1: '?',r3==1 ? tua_toint(t,pos3) : v3);
            // call_opi(t, bytecode.opcode, pos1, r2,r2==1?pos2:v2, r3,r3==1 ? pos3 : v3);
            // printf("call after op:%s R%c=%d R%c=%d R%c=%d\n", opcode_tostr(instr.opcode), pos1+'a'-1,tua_toint(t,pos1), pos2+'a'-1,tua_toint(t,pos2), r3==1? pos3+'a'-1: '?',r3==1 ? tua_toint(t,pos3) : v3 );
            break;
        }
        case OP_JZ:
        {
            break;
        }
        case OP_FORPREP: 
        {
            int a = GETARG_A(i);
            int limit = VALUE(a+1);
            int step = VALUE(a+2);
            // int count = RAV(i) - RBV(i);
            // RAV(i) = count;
            // if(count<=0) {
            //     int k = GETARG_C(i);
            //     pc+=k;
            // }

            // pc += GETARG_B(i);
            RAV(i) -= step;
            debug("OP_FORPREP R%d=%d jmp: %d\n", a,RAV(i), GETARG_B(i));

            pc = savedpc + GETARG_B(i);
            break;
        }
        case OP_FORLOOP: {
            int a = GETARG_A(i);
            int limit = VALUE(a+1);
            int step = VALUE(a+2);
            debug("OP_FORLOOP %d > %d,%d next:%d\n", RAV(i),limit,step,GETARG_B(i));
            if(RAV(i)>limit) {
                RAV(i) += step;
                // pc -= GETARG_B(i);
                pc = savedpc + GETARG_B(i);
            }else {
                
            }
            
            break;
        }
        case OP_JMP: {
            
            break;
        }
        case OP_JE: {
            if(RAV(i) == RBV(i)) {
            }else {
                pc = savedpc + GETARG_C(i);
            }
            debug("OP_JE %d == %d\n", RAV(i), RBV(i));
            break;
        }
        case OP_JLT: {
            if(RAV(i) < RBV(i)) {
            }else {
                pc = savedpc + GETARG_C(i);
            }
            debug("OP_JLT %d < %d\n", RAV(i), RBV(i));
            break;
        }
        case OP_LABEL:
            
            break;
      
        case OP_CALL: {
            // tua_stack_id top;
            int ra = RAV(i); //pc offset
            int nparam = GETARG_B(i);
            int nres = GETARG_C(i) - 1;
            if(nparam != 0) {
                // t->st_top = base + ra + nparam;
            }
            //get closure info by ra
            int maxstacksize = 5; 
            //savepc(t);//call func max stack size
            ci->fn.t.savedpc = pc;
            t->savedpc = pc;

            tua_call_info *newci;
            if(ci->next != NULL) {
                newci = ci->next;
            }else {
                newci = tua_malloc(t,sizeof(tua_call_info));
                newci->next = NULL;
                ci->next = newci;
            }
            newci->st_start = ci->st_start+GETARG_A(i)+1;
            newci->st_top = ci->st_top+1+maxstacksize;
            newci->fn.t.savedpc = savedpc + ra;
            newci->prev = ci;
            newci->nresults = nres;
            
            
            ci = newci;
            // printf("OP_CALL %d=%d\n", GETARG_A(i),ra);
            goto startfunc;
        }
        case OP_LOADENV: {
            printf("LOADENV\n");
            
            break;
        }
        case OP_LOADFUNC: {
            RAV(i) = GETARG_B(i);
            debug("OP_LOADFUNC %d=%d\n", GETARG_A(i), GETARG_B(i));

            break;
        }
        case OP_PRINT: {
            int v = RAV(i);
            printf("PRINT: R%d=%d\n", GETARG_A(i), v);
            
            break;
        }
        
        default:
            break;
        }
    }
    printf("return cnt:%d\n", cnt);
    return 0;
}



void * tua_malloc(tua_state *t, size_t size) {
    printf("tua_malloc %ld\n", size);
    return malloc(size);
    // return pool_falloc(t->pool, size);
}