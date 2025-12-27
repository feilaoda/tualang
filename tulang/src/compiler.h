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
} Compiler;

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

// Function to compile statements
void compileStmt(Compiler* compiler, Stmt* stmt);
void compileIfStmt(Compiler* compiler, IfStmt* stmt);
void compileForStmt(Compiler* compiler, ForStmt* stmt);
void compileForInStmt(Compiler* compiler, ForInStmt* stmt);
void compileBlockStmt(Compiler* compiler, BlockStmt* stmt);
void compileReturnStmt(Compiler* compiler, ReturnStmt* stmt);
void compileExprStmt(Compiler* compiler, ExprStmt* stmt);
void compileVarStmt(Compiler* compiler, VarStmt* stmt);
void compileFuncStmt(Compiler* compiler, FuncStmt* stmt);
void compileStructStmt(Compiler* compiler, StructStmt* stmt);
void compileObjectStmt(Compiler* compiler, ObjectStmt* stmt);
void compileEnumStmt(Compiler* compiler, EnumStmt* stmt);
void initCompiler(Compiler* compiler);
// Helper functions
int makeLabel(Compiler* compiler);

Value tokenToValue(Token token);

#endif
