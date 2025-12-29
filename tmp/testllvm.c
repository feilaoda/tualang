#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <llvm-c/Core.h>
#include <llvm-c/Analysis.h>


int main() {
    // Initialize LLVM
    LLVMContextRef context = LLVMContextCreate();
    LLVMModuleRef module = LLVMModuleCreateWithNameInContext("tua_module", context);
    LLVMBuilderRef builder = LLVMCreateBuilderInContext(context);

    // Declare printf function
    LLVMTypeRef printfParamTypes[] = { LLVMPointerType(LLVMInt8TypeInContext(context), 0) };
    LLVMTypeRef printfType = LLVMFunctionType(LLVMInt32TypeInContext(context), 
                                             printfParamTypes, 1, 1);
    LLVMValueRef printfFunc = LLVMAddFunction(module, "printf", printfType);


    // Create main function type (int main())
    LLVMTypeRef returnType = LLVMInt32TypeInContext(context);
    LLVMTypeRef mainFuncType = LLVMFunctionType(returnType, NULL, 0, 0);
    LLVMValueRef mainFunc = LLVMAddFunction(module, "main", mainFuncType);

    // Create entry block
    LLVMBasicBlockRef entry = LLVMAppendBasicBlock(mainFunc, "entry");
    LLVMPositionBuilderAtEnd(builder, entry);
 // Create string constant for printf

    LLVMValueRef formatStr = LLVMBuildGlobalStringPtr(builder, "Hello, World!\n", "str");
    
       // Call printf
    LLVMValueRef args[] = { formatStr };
    LLVMBuildCall2(builder,printfType, printfFunc, args, 1, "");

    // int a = 10;
    LLVMValueRef a = LLVMBuildAlloca(builder, LLVMInt32TypeInContext(context), "a");
    LLVMValueRef ten = LLVMConstInt(LLVMInt32TypeInContext(context), 1, 0);
    LLVMBuildStore(builder, ten, a);

    // a = a + 20
    // LLVMValueRef loadA = LLVMBuildLoad2(builder, LLVMInt32TypeInContext(context), a, "load_a");
    // LLVMValueRef twenty = LLVMConstInt(LLVMInt32TypeInContext(context), 20, 0);
    // LLVMValueRef sum = LLVMBuildAdd(builder, loadA, twenty, "add");
    // LLVMBuildStore(builder, sum, a);



 // Create basic blocks
LLVMBasicBlockRef loopCond = LLVMAppendBasicBlock(mainFunc, "loop.cond");
    LLVMBasicBlockRef loopBody = LLVMAppendBasicBlock(mainFunc, "loop.body");
    LLVMBasicBlockRef loopInc = LLVMAppendBasicBlock(mainFunc, "loop.inc");
    LLVMBasicBlockRef loopEnd = LLVMAppendBasicBlock(mainFunc, "loop.end");

    // Initialize i = 0
    LLVMValueRef i = LLVMBuildAlloca(builder, LLVMInt32TypeInContext(context), "i");
    LLVMValueRef zero = LLVMConstInt(LLVMInt32TypeInContext(context), 0, 0);
    LLVMBuildStore(builder, zero, i);
    LLVMBuildBr(builder, loopCond);

    // Loop condition: i < 100000000
    LLVMPositionBuilderAtEnd(builder, loopCond);
    LLVMValueRef loadI = LLVMBuildLoad2(builder, LLVMInt32TypeInContext(context), i, "i.val");
    LLVMValueRef limit = LLVMConstInt(LLVMInt32TypeInContext(context), 100000000, 0);
    LLVMValueRef cond = LLVMBuildICmp(builder, LLVMIntSLT, loadI, limit, "cmp");
    
    LLVMBuildCondBr(builder, cond, loopBody, loopEnd);

    // Loop body: a = a + i
    LLVMPositionBuilderAtEnd(builder, loopBody);
    LLVMValueRef loadA = LLVMBuildLoad2(builder, LLVMInt32TypeInContext(context), a, "a.val");
    LLVMValueRef loadIBody = LLVMBuildLoad2(builder, LLVMInt32TypeInContext(context), i, "i.body");
    LLVMValueRef sum = LLVMBuildAdd(builder, loadA, loadIBody, "add");
    LLVMBuildStore(builder, sum, a);
    LLVMBuildBr(builder, loopInc);

    // Loop increment: i++
    LLVMPositionBuilderAtEnd(builder, loopInc);
    LLVMValueRef inc = LLVMBuildAdd(builder, loadI, LLVMConstInt(LLVMInt32TypeInContext(context), 1, 0), "inc");
    LLVMBuildStore(builder, inc, i);
    LLVMBuildBr(builder, loopCond);

    // Loop end
    LLVMPositionBuilderAtEnd(builder, loopEnd);

    // printf("%d", a)
    LLVMValueRef formatStr2 = LLVMBuildGlobalStringPtr(builder, "%d\n", "fmt");
    LLVMValueRef printVal = LLVMBuildLoad2(builder, LLVMInt32TypeInContext(context), a, "print_val");
    LLVMValueRef args2[] = { formatStr2, printVal };
    LLVMBuildCall2(builder, printfType, printfFunc, args2, 2, "");

    // Add return 0
    LLVMValueRef returnValue = LLVMConstInt(LLVMInt32TypeInContext(context), 0, 0);
    LLVMBuildRet(builder, returnValue);

    // Verify module
    char *error = NULL;
    LLVMVerifyModule(module, LLVMAbortProcessAction, &error);
    LLVMDisposeMessage(error);

    // Print generated IR
    char *ir = LLVMPrintModuleToString(module);
    printf("%s\n", ir);
    // Print IR to file
    if (LLVMPrintModuleToFile(module, "bin/output.ll", &error) != 0) {
        fprintf(stderr, "Error printing IR to file: %s\n", error);
        LLVMDisposeMessage(error);
        return 1;
    }

   
    LLVMDisposeMessage(ir);

    // Cleanup
    LLVMDisposeBuilder(builder);
    LLVMDisposeModule(module);
    LLVMContextDispose(context);

    return 0;
}