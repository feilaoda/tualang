#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "lexer.h"
#include "parser.h"
#include "compiler.h"
#include "debug.h"

#include "llvm/llvm.h"
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
#ifdef DEBUG
    printf("result: %d\n", result);
#endif
    // Cleanup
    LLVMDisposeExecutionEngine(engine);
    return result;
}

void cleanup(LLVMModuleRef module, LLVMBuilderRef builder, LLVMContextRef context, char* ir) {
    if (ir) {
#ifdef DEBUG
        printf("Disposing IR...\n");
#endif
        LLVMDisposeMessage(ir);
    }

    if (builder) {
#ifdef DEBUG
        printf("Disposing builder...\n");
#endif
        LLVMDisposeBuilder(builder);
    }

    // if (module) {
    //     printf("Disposing module...\n");
    //     LLVMDisposeModule(module);
    // }

    if (context) {
#ifdef DEBUG
        printf("Disposing context...\n");
#endif
        LLVMContextDispose(context);
    }
}

void endLLVM(Compiler* compiler) {
    debug("endLLVM\n");
    LLVMContextRef context = compiler->context;
    LLVMBuilderRef builder = compiler->builder;
    LLVMModuleRef module = compiler->module;

    // Add implicit `return 0` for the generated `main` if needed.
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(builder))) {
        debug("call return\n");
        LLVMValueRef returnValue = LLVMConstInt(LLVMInt32TypeInContext(context), 0, 0);
        LLVMBuildRet(builder, returnValue);
    }

    // Verify module
    char *error = NULL;
    debug("call print error\n");
    if (LLVMVerifyModule(module, LLVMReturnStatusAction, &error) != 0) {
        fprintf(stderr, "LLVMVerifyModule failed: %s\n", error ? error : "(unknown)");
        LLVMDisposeMessage(error);
        cleanup(module, builder, context, NULL);
        return;
    }

#ifdef DEBUG
    debug("call print IR\n");
    char *ir = LLVMPrintModuleToString(module);
    printf("%s\n", ir);
    if (LLVMPrintModuleToFile(module, "bin/output.ll", &error) != 0) {
        fprintf(stderr, "Error printing IR to file: %s\n", error);
        LLVMDisposeMessage(error);
        cleanup(module, builder, context, ir);
        return;
    }
    struct timeval stop, start;
    gettimeofday(&start, NULL);
    executeModule(module);
    gettimeofday(&stop, NULL);
    printf("====result0: time: %fs\n",(float)((stop.tv_sec - start.tv_sec) * 1000000 + stop.tv_usec - start.tv_usec)/1000000.0);
    cleanup(module, builder, context, ir);
#else
    executeModule(module);
    cleanup(module, builder, context, NULL);
#endif

}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <source file>\n", argv[0]);
        return 1;
    }
    char* source = readFile(argv[1]);
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

#ifdef DEBUG
    printf("Parsing succeeded.\n");
#endif

    Compiler compiler;
    initCompiler(&compiler);
    initLLVM(&compiler);
    for(int i = 0; i < statements->length; i++) {
        Stmt* stmt = listGet(statements, i);
#ifdef DEBUG
        printf("stmt: %s\n", stmtTypeToString(stmt->type));
#endif
        compileStmt(&compiler, stmt);
    }

    endLLVM(&compiler);

    free(source);
    return 0;
}
