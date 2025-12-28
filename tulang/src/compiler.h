#ifndef COMPILER_H
#define COMPILER_H
#include "opcode.h"
#include "parser.h"
#include <stddef.h>
#include <stdint.h>

#include <llvm-c/Core.h>
#include <llvm-c/Analysis.h>

typedef struct {
    Token name;
    int depth;
    int isConst;
} Local;


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
    List* code;          // Bytecode instructions
    List* constants;     // Constant pool
    
    // Local variable management
    Local* locals;      
    int localCount;
    int maxLocals;
    
    // Scope management
    int scopeDepth;     
    
    // Function compilation
    struct Compiler* enclosing;  // Enclosing function compiler
    FuncStmt* function;         // Current function being compiled
    
    // Loop and jump tracking
    List* loops;        // Stack of current loops
    List* breaks;       // Stack of break statements
    int labelCount;     // For generating unique labels
    
    // Debug information  
    bool hadError;
    bool panicMode;

    List* ir;
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
    LLVMTypeRef tuaValueType; // cached {i32, i64} tagged value type
    List* closureSigs;       // List<ClosureSig*>, variable name -> function type
    List* closureReturnSigs; // List<ClosureReturnSig*>, function name -> closure return signature
    LLVMTypeRef lastLambdaFuncType; // side-channel: funcType of last compiled lambda expr
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
    ALIAS_OBJECT
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


typedef struct {
    OpCode op;
    int offset;
} Jump;

typedef struct {
    OpCode op;
    int offset;
    int loopStart;
} Loop;

typedef struct {
    OpCode op;
    int offset;
    int label;
} Label;

typedef struct {
    OpCode op;
    int offset;
    int label;
    int loopStart;
} LoopLabel;

typedef struct {
    OpCode op;
    int offset;
    int label;
    int loopStart;
    int exitJump;
} LoopExit;

typedef struct {
    OpCode op;
    int offset;
    int label;
    int exitJump;
} LoopExitLabel;

typedef enum {
    VAL_INT,
    VAL_DOUBLE,
    VAL_LONG,
    VAL_STRING,
    VAL_BOOL,
    VAL_NIL,
    VAL_OBJ
} ValueType;

typedef enum {
    OBJ_STRING,
    OBJ_FUNCTION,
    OBJ_CLOSURE,
    OBJ_UPVALUE
} ObjType;

typedef struct Obj {
    ObjType type;          // Type tag
    bool isMarked;         // For GC
    struct Obj* next;      // For GC linked list
} Obj;
typedef struct {
    ValueType type;
    union {
        int32_t i;
        int64_t l;
        double d;
        char* string;
        bool boolean;
        Obj* obj;
    } as;
} Value;



typedef struct {
    Obj obj;              // Base object header
    int length;           // String length
    char* chars;          // String data
    uint32_t hash;        // Hash value
} ObjString;

typedef struct {
    Obj obj;              // Base object header
    Token name;           // Function name
    int arity;            // Parameter count
    List* code;           // Bytecode
    List* constants;      // Constants pool
    int maxLocals;        // Max local variables
    int maxStack;         // Max stack size
    List* lines;          // Debug line info
    int upvalueCount;     // Number of upvalues
} ObjFunction;

typedef struct ObjUpvalue {
    Obj obj;              // Base object header
    Value* location;      // Pointer to variable location
    Value closed;         // Value after closing
    struct ObjUpvalue* next;  // Link to next upvalue
} ObjUpvalue;

typedef struct ObjClosure {
    Obj obj;              // Base object header
    ObjFunction* function; // Enclosed function
    ObjUpvalue** upvalues; // Array of upvalues
    int upvalueCount;     // Number of upvalues
} ObjClosure;



typedef struct {
    char* text;
    int indent;
} IRLine;

LLVMValueRef compileExpr(Compiler* compiler, Expr* expr);
LLVMValueRef compileExprMulti(Compiler* compiler, Expr* expr);

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
// Helper functions
int makeLabel(Compiler* compiler);

Value tokenToValue(Token token);

LLVMTypeRef compilerGetClosureType(Compiler* compiler);
LLVMTypeRef compilerGetMapType(Compiler* compiler);
LLVMTypeRef compilerGetTuaValueType(Compiler* compiler);
void compilerRegisterClosureSig(Compiler* compiler, const char* name, int nameLen, LLVMTypeRef funcType);
LLVMTypeRef compilerFindClosureSig(Compiler* compiler, const char* name, int nameLen);
LLVMTypeRef compilerClosureSigFromType(Compiler* compiler, Type* type);
void compilerRegisterClosureReturnSig(Compiler* compiler, const char* name, int nameLen, LLVMTypeRef funcType);
LLVMTypeRef compilerFindClosureReturnSig(Compiler* compiler, const char* name, int nameLen);

#endif
