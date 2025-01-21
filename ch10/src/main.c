#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lexer.h"
#include "parser.h"
#include "compiler.h"
#include "debug.h"
#include "llvm.h"
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>

static char* readFile(const char* path) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "Could not open file \"%s\".\n", path);
        exit(74);
    }

    fseek(file, 0L, SEEK_END);
    size_t fileSize = ftell(file);
    rewind(file);

    char* buffer = (char*)malloc(fileSize + 1);
    if (buffer == NULL) {
        fprintf(stderr, "Not enough memory to read \"%s\".\n", path);
        exit(74);
    }

    size_t bytesRead = fread(buffer, sizeof(char), fileSize, file);
    if (bytesRead < fileSize) {
        fprintf(stderr, "Could not read file \"%s\".\n", path);
        exit(74);
    }

    buffer[bytesRead] = '\0';

    fclose(file);
    return buffer;
}

void initLLVM(Compiler* compiler) {
    // LLVMInitializeNativeTarget();
    // LLVMInitializeNativeAsmPrinter();
    // LLVMInitializeNativeAsmParser();
    LLVMLinkInMCJIT();
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();

    LLVMContextRef context = LLVMContextCreate();
    LLVMModuleRef module = LLVMModuleCreateWithNameInContext("tua_module", context);
    LLVMBuilderRef builder = LLVMCreateBuilderInContext(context);

    // Declare printf function
    // LLVMTypeRef printfParamTypes[] = { LLVMPointerType(LLVMInt8TypeInContext(context), 0) };
    // LLVMTypeRef printfType = LLVMFunctionType(LLVMInt32TypeInContext(context), 
    //                                          printfParamTypes, 1, 1);
    // LLVMValueRef printfFunc = LLVMAddFunction(module, "printf", printfType);


    // Create main function type (int main())
    LLVMTypeRef returnType = LLVMInt32TypeInContext(context);
    LLVMTypeRef mainFuncType = LLVMFunctionType(returnType, NULL, 0, 0);
    LLVMValueRef mainFunc = LLVMAddFunction(module, "main", mainFuncType);

    // Create entry block
    LLVMBasicBlockRef entry = LLVMAppendBasicBlock(mainFunc, "entry");
    LLVMPositionBuilderAtEnd(builder, entry);

    compiler->context = context;
    compiler->builder = builder;
    compiler->module = module;
    Block * block = malloc(sizeof(Block));
    block->parent = NULL;
    block->func = mainFunc;
    block->variables = listNew();
    compiler->current = block;
}

int executeModule(LLVMModuleRef module) {
    char *error = NULL;
    
    // Create execution engine
    LLVMExecutionEngineRef engine;
    if (LLVMCreateExecutionEngineForModule(&engine, module, &error) != 0) {
        fprintf(stderr, "Failed to create execution engine: %s\n", error);
        LLVMDisposeMessage(error);
        return 1;
    }

    // Find main function
    LLVMValueRef mainFunc = LLVMGetNamedFunction(module, "main");
    if (!mainFunc) {
        fprintf(stderr, "No main function found\n");
        return 1;
    }

    // Execute main function
    int (*mainFn)(void) = (int (*)(void))LLVMGetFunctionAddress(engine, "main");
    int result = mainFn();
    printf("result: %d\n", result);
    // Cleanup
    LLVMDisposeExecutionEngine(engine);
    return result;
}

void cleanup(LLVMModuleRef module, LLVMBuilderRef builder, LLVMContextRef context, char* ir) {
    if (ir) {
        printf("Disposing IR...\n");
        LLVMDisposeMessage(ir);
    }

    if (builder) {
        printf("Disposing builder...\n");
        LLVMDisposeBuilder(builder);
    }

    // if (module) {
    //     printf("Disposing module...\n");
    //     LLVMDisposeModule(module);
    // }

    if (context) {
        printf("Disposing context...\n");
        LLVMContextDispose(context);
    }
}

void endLLVM(Compiler* compiler) {
    debug("endLLVM\n");
    LLVMContextRef context = compiler->context;
    LLVMBuilderRef builder = compiler->builder;
    LLVMModuleRef module = compiler->module;

 // Declare printf function
    LLVMTypeRef printfParamTypes[] = { LLVMPointerType(LLVMInt8TypeInContext(context), 0) };
    LLVMTypeRef printfType = LLVMFunctionType(LLVMInt32TypeInContext(context), 
                                             printfParamTypes, 1, 1);
    LLVMValueRef printfFunc = LLVMAddFunction(module, "printf", printfType);
    VariableRef var = findVariable(compiler->current->variables, "a");
// printf("%d", a)
    debug("call printf\n");
    LLVMValueRef formatStr2 = LLVMBuildGlobalStringPtr(builder, "%d\n", "fmt");
    LLVMValueRef printVal = LLVMBuildLoad2(builder, LLVMInt32TypeInContext(context), var.value, "print_val");
    LLVMValueRef args2[] = { formatStr2, printVal };
    LLVMBuildCall2(builder, printfType, printfFunc, args2, 2, "");


     // Add return 0
    debug("call return\n");
    LLVMValueRef returnValue = LLVMConstInt(LLVMInt32TypeInContext(context), 0, 0);
    LLVMBuildRet(builder, returnValue);

    // Verify module
    char *error = NULL;
    debug("call print error\n");

    LLVMVerifyModule(module, LLVMAbortProcessAction, &error);

    LLVMDisposeMessage(error);

    debug("call print IR\n");
    // Print generated IR
    char *ir = LLVMPrintModuleToString(module);
    printf("%s\n", ir);
    // Print IR to file
    if (LLVMPrintModuleToFile(module, "bin/output.ll", &error) != 0) {
        fprintf(stderr, "Error printing IR to file: %s\n", error);
        LLVMDisposeMessage(error);
        return;
    }

   
    executeModule(module);

    // printf("free ir\n"); 
    // LLVMDisposeMessage(ir);

    // printf("free builder\n");
    // // Cleanup
    // LLVMDisposeBuilder(builder);
    // printf("free module\n");
    // LLVMDisposeModule(module);
    // printf("free context\n");
    // LLVMContextDispose(context);

    // system("gcc -O3 bin/output.ll -o bin/output.bin");
    cleanup(module, builder, context, ir);

}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <source file>\n", argv[0]);
        return 1;
    }
    char * file = argv[1];
    char* source = readFile(argv[1]);
    FILE* target = fopen("examples/test5.tasm", "w+");
    if (target == NULL) {
        fprintf(stderr, "Could not open file \"%s\".\n", "examples/test5.tasm");
        exit(74);
    }
    Lexer lexer;
    initLexer(&lexer, source);

    Parser parser;
    initParser(&parser, &lexer);
    List*statements;
    bool success = parse(&parser,&statements);

    if (!success) {
        fprintf(stderr, "Parsing failed.\n");
        free(source);
        return 1;
    }

    printf("Parsing succeeded.\n");

    Compiler compiler;
    initCompiler(&compiler);
    initLLVM(&compiler);
    for(int i = 0; i < statements->length; i++) {
        Stmt* stmt = listGet(statements, i);
        printf("stmt: %s\n", stmtTypeToString(stmt->type));
        compileStmt(&compiler, stmt);
    }

    // for(int i = 0; i< compiler.ir->length; i++) {
    //     printf("%s\n", ((IRLine*)listGet(compiler.ir, i))->text);
    //     fwrite(((IRLine*)listGet(compiler.ir, i))->text, 1, strlen(((IRLine*)listGet(compiler.ir, i))->text), target);
    // }

    endLLVM(&compiler);

    free(source);
    return 0;
}