#include "llvm.h"
#include "compiler.h"
#include "debug.h"

LLVMTypeRef buildMethodType() {
    //TODO
    return NULL;
}

// LLVMValueRef emitCallExpr2(Compiler* compiler, CallExpr* expr) {
//     // Get object type and method
//     VariableRef obj = findVariableExpr(compiler, expr->caller);
//     char* methodName = getMethodName(expr->callee);
    
//     // Create struct type with vtable
//     LLVMTypeRef vtableType = LLVMStructCreateNamed(compiler->context, "vtable");
//     LLVMTypeRef objType = LLVMStructCreateNamed(compiler->context, "object");
    
//     // Add vtable pointer to object struct
//     LLVMTypeRef elementTypes[] = {
//         LLVMPointerType(vtableType, 0), // vtable ptr
//         // ... other fields
//     };
//     LLVMStructSetBody(objType, elementTypes, 1, false);
    
//     // Get method from vtable
//     LLVMValueRef vtablePtr = LLVMBuildStructGEP2(
//         compiler->builder, 
//         objType,
//         obj.value, 
//         0, 
//         "vtable"
//     );
    
//     // Build method call with this pointer
//     LLVMValueRef method = LLVMBuildLoad2(
//         compiler->builder,
//         LLVMPointerType(
//             buildMethodType(),
//             // LLVMFunctionType(
//             // LLVMVoidType(compiler->context), 
//             // NULL, 
//             // 0, 
//             // false
//             // ), 
//         0),
//         vtablePtr,
//         "method"
//     );
    
//     // Add this as first argument
//     LLVMValueRef args[expr->arguments->length + 1];
//     args[0] = obj.value; // this pointer
    
//     // Add other arguments
//     ListNode* arg = expr->arguments->head;
//     int i = 1;
//     while (arg != NULL) {
//         args[i++] = compileExpr(compiler, (Expr*)arg->data);
//         arg = arg->next; 
//     }
    
//     // Call method
//     return LLVMBuildCall2(
//         compiler->builder,
//         LLVMTypeOf(method),
//         method,
//         args,
//         expr->arguments->length + 1,
//         "call"
//     );
// }



LLVMValueRef emitCallExpr(Compiler* compiler, CallExpr* expr) {
    emitDebug("emitCallExpr\n");

    return NULL;
    // LLVMBuilderRef builder = compiler->builder;
    // Block* block = compiler->current;

    // // Lookup function value
    // VariableRef func = findVariableExpr(compiler, expr->callee);
    // if (!func.value) {
    //     emitDebug("Undefined function");
    //     return NULL;
    // }

    // // // Compile arguments
    // // LLVMValueRef* args = NULL;
    // // int argCount = 0;
    // // if (expr->arguments != NULL) {
    // //     args = malloc(sizeof(LLVMValueRef) * expr->arguments->length);
    // //     ListNode* arg = expr->arguments->head;
    // //     while (arg != NULL) {
    // //         args[argCount++] = emitExpr(compiler, (Expr*)arg->data);
    // //         arg = arg->next;
    // //     }
    // // }

    // // // Build call instruction
    // // LLVMValueRef call = LLVMBuildCall2(
    // //     builder,
    // //     LLVMTypeOf(func.value),
    // //     func.value,
    // //     args,
    // //     argCount,
    // //     "call"
    // // );

    // // // Cleanup
    // // if (args) {
    // //     free(args);
    // // }

    // // 使用栈内存存储参数，避免频繁的堆内存分配
    // LLVMValueRef stackArgs[64];  // 支持最多64个参数
    // LLVMValueRef* args = stackArgs;
    // int argCount = 0;

    // if (expr->arguments) {
    //     if (expr->arguments->length > 64) {
    //         emitDebug("Too many arguments (max 64)");
    //         return NULL;
    //     }

    //     // 编译参数
    //     for (ListNode* arg = expr->arguments->head; arg != NULL; arg = arg->next) {
    //         LLVMValueRef argValue = emitExpr(compiler, (Expr*)arg->data);
    //         if (!argValue) {
    //             emitDebug("Failed to compile argument");
    //             return NULL;
    //         }
    //         args[argCount++] = argValue;
    //     }
    // }

    // // Build call instruction
    // LLVMValueRef call = LLVMBuildCall2(
    //     builder,
    //     LLVMTypeOf(func.value),
    //     func.value,
    //     args,
    //     argCount,
    //     "call"
    // );

    // return call;
}
