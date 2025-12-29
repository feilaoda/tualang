#ifndef COMPILER_H
#define COMPILER_H
#include "parser.h"
#include <stddef.h>
#include <stdint.h>

#include <llvm-c/Core.h>
#include <llvm-c/Analysis.h>
#include <stdarg.h>

typedef struct Block {
    struct Block* parent;
    LLVMValueRef func;
    List* variables;
    List* labels; // List<LabelInfo*>
}Block;

typedef struct ForBlock {
    Block block;
    LLVMBasicBlockRef loopCond;
    LLVMBasicBlockRef loopBody;
    LLVMBasicBlockRef loopInc;
    LLVMBasicBlockRef loopEnd;
} ForBlock;

typedef struct Compiler{
    // Debug information  
    bool hadError;
    bool panicMode;

    const char* currentFilePath; // for diagnostics (may be NULL)

    LLVMBuilderRef builder;
    LLVMContextRef context;
    LLVMModuleRef module;
    Block *current;

    List* structs; // List<StructInfo*>
    List* enums;   // List<EnumInfo*>

    List* loopStack; // List<LoopTarget*>

    const char* currentModulePrefix;
    int currentModulePrefixLen;
    List* currentAliases; // List<SymbolAlias*>

    // Multi-return function tracking (LLVM JIT path)
    List* multiReturns; // List<MultiReturnInfo*>
    int wantMultiValue; // when true, calls return full tuple value

    // Closure / lambda (LLVM JIT path)
    int boxAllLocals;        // when true, locals/params stored in heap boxes
    int lambdaCount;         // unique lambda id
	    LLVMTypeRef closureType; // cached {ptr,ptr} closure value type
	    LLVMTypeRef mapType;     // cached %tua_map* type
	    LLVMTypeRef arrayType;   // cached %tua_array* type
	    LLVMTypeRef tuaValueType; // cached {i32, i64} tagged value type
	    List* closureSigs;       // List<ClosureSig*>, variable name -> function type
	    List* closureReturnSigs; // List<ClosureReturnSig*>, function name -> closure return signature
	    LLVMTypeRef lastLambdaFuncType; // side-channel: funcType of last compiled lambda expr

    const char* lastSetFilePath; // last emitted runtime file path (may be NULL)
    int lastSetLine;             // last emitted runtime line
    int lastSetCol;              // last emitted runtime column

	    // Side-channel: when compiling a map literal for a typed map variable, enforce K/V.
	    LLVMTypeRef expectedMapKeyType;
	    LLVMTypeRef expectedMapValueType;

	    // Side-channel: when compiling an array literal for a typed array variable, enforce element type/length.
	    LLVMTypeRef expectedArrayElemType;
	    int64_t expectedArrayFixedLen; // -1 => dynamic / unknown

    // Tail recursion elimination (self tail calls) state for the currently compiled function.
    // Kept opaque here; implemented in `src/compiler.c`.
    struct TailrecState* tailrec;
} Compiler;

typedef struct MultiReturnInfo {
    char* name;
    int nameLen;
    int count;
} MultiReturnInfo;

typedef struct ClosureSig {
    char* name;
    int nameLen;
    LLVMTypeRef funcType; // the lambda implementation function type (env + args)
} ClosureSig;

typedef struct ClosureReturnSig {
    char* name;
    int nameLen;
    LLVMTypeRef funcType; // expected closure call signature (env + args) for function return value
} ClosureReturnSig;

typedef struct LoopTarget {
    LLVMBasicBlockRef breakTarget;
    LLVMBasicBlockRef continueTarget;
} LoopTarget;

typedef struct LabelInfo {
    char* name;
    int length;
    LLVMBasicBlockRef block;
    int isDefined;
} LabelInfo;

typedef enum SymbolAliasKind {
    ALIAS_FUNC = 0,
    ALIAS_STRUCT,
    ALIAS_ENUM,
    ALIAS_OBJECT,
    ALIAS_MODULE
} SymbolAliasKind;

typedef struct SymbolAlias {
    char* local;
    int localLen;
    char* qualified;
    int qualifiedLen;
    SymbolAliasKind kind;
} SymbolAlias;

typedef struct StructInfo StructInfo;
typedef struct EnumInfo EnumInfo;

SymbolAlias* compilerFindAlias(Compiler* compiler, const char* local, int localLen);
StructInfo* compilerResolveStructByToken(Compiler* compiler, const Token* name);
EnumInfo* compilerResolveEnumByToken(Compiler* compiler, const Token* name);
char* compilerQualifyToken(Compiler* compiler, const Token* name, int* outLen);

int compilerMultiReturnCount(Compiler* compiler, const char* name, int nameLen);
void compilerRegisterMultiReturn(Compiler* compiler, const char* name, int nameLen, int count);

typedef struct StructInfo {
    char* name;
    int nameLength;
    LLVMTypeRef type;
    StructStmt* decl;
} StructInfo;

StructInfo* compilerFindStruct(Compiler* compiler, const char* name, int length);

typedef struct EnumInfo {
    char* name;
    int nameLength;
    EnumStmt* decl;
    int isStringTag; // 0=int tag, 1=string tag
} EnumInfo;

EnumInfo* compilerFindEnum(Compiler* compiler, const char* name, int length);


LLVMValueRef compileExpr(Compiler* compiler, Expr* expr);
LLVMValueRef compileExprMulti(Compiler* compiler, Expr* expr);

void compilerErrorAt(Compiler* compiler, int line, const char* fmt, ...);
void compilerErrorAtEx(Compiler* compiler, const char* file, int line, int col, const char* fmt, ...);
void compilerErrorAtToken(Compiler* compiler, const Token* token, const char* fmt, ...);

// Function to compile statements
void compileStmt(Compiler* compiler, Stmt* stmt);
void compileIfStmt(Compiler* compiler, IfStmt* stmt);
void compileForStmt(Compiler* compiler, ForStmt* stmt);
void compileForInStmt(Compiler* compiler, ForInStmt* stmt);
void compileWhileStmt(Compiler* compiler, WhileStmt* stmt);
void compileDoWhileStmt(Compiler* compiler, DoWhileStmt* stmt);
void compileBreakStmt(Compiler* compiler, BreakStmt* stmt);
void compileContinueStmt(Compiler* compiler, ContinueStmt* stmt);
void compileLabelStmt(Compiler* compiler, LabelStmt* stmt);
void compileGotoStmt(Compiler* compiler, GotoStmt* stmt);
void compileBlockStmt(Compiler* compiler, BlockStmt* stmt);
void compileReturnStmt(Compiler* compiler, ReturnStmt* stmt);
void compileExprStmt(Compiler* compiler, ExprStmt* stmt);
void compileVarStmt(Compiler* compiler, VarStmt* stmt);
void compileDestructureStmt(Compiler* compiler, DestructureStmt* stmt);
void compileFuncStmt(Compiler* compiler, FuncStmt* stmt);
void compileStructStmt(Compiler* compiler, StructStmt* stmt);
void compileImplStmt(Compiler* compiler, ImplStmt* stmt);
void compileObjectStmt(Compiler* compiler, ObjectStmt* stmt);
void compileEnumStmt(Compiler* compiler, EnumStmt* stmt);
void initCompiler(Compiler* compiler);

	LLVMTypeRef compilerGetClosureType(Compiler* compiler);
	LLVMTypeRef compilerGetMapType(Compiler* compiler);
	LLVMTypeRef compilerGetArrayType(Compiler* compiler);
	LLVMTypeRef compilerGetTuaValueType(Compiler* compiler);
	LLVMTypeRef compilerGetOptionType(Compiler* compiler, LLVMTypeRef inner);
void compilerRegisterClosureSig(Compiler* compiler, const char* name, int nameLen, LLVMTypeRef funcType);
LLVMTypeRef compilerFindClosureSig(Compiler* compiler, const char* name, int nameLen);
LLVMTypeRef compilerClosureSigFromType(Compiler* compiler, Type* type);
void compilerRegisterClosureReturnSig(Compiler* compiler, const char* name, int nameLen, LLVMTypeRef funcType);
LLVMTypeRef compilerFindClosureReturnSig(Compiler* compiler, const char* name, int nameLen);

#endif
