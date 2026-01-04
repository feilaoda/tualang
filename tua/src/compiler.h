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
    List* traits;  // List<TraitInfo*>
    List* enums;   // List<EnumInfo*>
    // Generic function templates registered per module (`fn f<T>(...) { ... }`).
    List* genericFuncTemplates; // List<GenericFuncTemplate*>
    // Active generic type substitutions while compiling a monomorphized instance.
    List* genericSubsts; // List<GenericSubst*>
    // Recorded trait impl pairs (`impl Trait for Struct`) for bounds checks.
    List* traitImplPairs; // List<TraitImplPair*>

    List* loopStack; // List<LoopTarget*>

    const char* currentModulePrefix;
    int currentModulePrefixLen;
    List* currentAliases; // List<SymbolAlias*>

    // When compiling an `object O { ... }` method, this is set to the (already-qualified)
    // object name so unqualified calls like `f()` inside the object can resolve to `O.f`.
    const char* currentObjectPrefix;
    int currentObjectPrefixLen;
    // Short (unmangled) current method name for disambiguation.
    const char* currentObjectMethodName;
    int currentObjectMethodNameLen;

    // Multi-return function tracking (LLVM JIT path)
    List* multiReturns; // List<MultiReturnInfo*>
    int wantMultiValue; // when true, calls return full tuple value

    // Closure / lambda (LLVM JIT path)
    int boxAllLocals;        // when true, locals/params stored in heap boxes
    // Escape analysis v1: when non-NULL, this function-scoped name set (List<Token*>) indicates
    // which locals/params must be boxed because they are captured by an escaping closure.
    // When set, codegen should box only these names (unless boxAllLocals is also set).
    List* boxedLocals;
    int lambdaCount;         // unique lambda id
	    LLVMTypeRef closureType; // cached {ptr,ptr} closure value type
	    LLVMTypeRef mapType;     // cached %tua_map* type
	    LLVMTypeRef arrayType;   // cached %tua_array* type
	    LLVMTypeRef bytesType;   // cached %tua_bytes* type
        // Cached Slice<T> struct types (keyed by element LLVMTypeRef).
        List* sliceTypes;       // List<SliceTypeEntry*>
        int sliceTypeCounter;
	    LLVMTypeRef tuaValueType; // cached {i32, i64} tagged value type
	    List* closureSigs;       // List<ClosureSig*>, variable name -> function type
	    List* closureReturnSigs; // List<ClosureReturnSig*>, function name -> closure return signature(s) for nested returns
	    LLVMTypeRef lastLambdaFuncType; // side-channel: funcType of last compiled lambda expr

    const char* lastSetFilePath; // last emitted runtime file path (may be NULL)
    int lastSetLine;             // last emitted runtime line
    int lastSetCol;              // last emitted runtime column

	    // Side-channel: when compiling a map literal for a typed map variable, enforce K/V.
	    LLVMTypeRef expectedMapKeyType;
	    LLVMTypeRef expectedMapValueType;
	    TypeKind expectedMapKeyKind;
	    TypeKind expectedMapValueKind;

	    // Side-channel: when compiling an array literal for a typed array variable, enforce element type/length.
	    LLVMTypeRef expectedArrayElemType;
	    TypeKind expectedArrayElemKind;
	    int64_t expectedArrayFixedLen; // -1 => dynamic / unknown

    // Tail recursion elimination (self tail calls) state for the currently compiled function.
    // Kept opaque here; implemented in `src/compiler.c`.
    struct TailrecState* tailrec;

    // LLVM optimization level for the generated module (0-3). Default: 0 (no extra passes).
    int llvmOptLevel;

    // When set, compile to native executable at this path instead of running via JIT.
    const char* outputPath;

    // AOT link options (passed to the system linker/clang when `--output` is used).
    // Stored as raw argv-backed strings; lifetime is the process lifetime.
    List* linkSearchPaths; // List<const char*> for -L
    List* linkLibs;        // List<const char*> for -l
    List* linkArgs;        // List<const char*> raw args (e.g. -Wl,... or /path/to/libfoo.a)

    // JIT dynamic library loads (POSIX-only for now).
    List* dlopenPaths;     // List<const char*>

    // `extern fn` declarations discovered during module loading (for diagnostics).
    // Element type is opaque to the core compiler; currently populated by `tuac` driver.
    List* externDecls;     // List<void*>

    // Script args for JIT execution (passed to generated `main(argc, argv)`).
    int runArgc;
    char** runArgv;

    // Performance/unsafe modes (must be explicitly enabled via CLI flags).
    int uncheckedIndex;      // when true, array indexing skips null/oob checks (UB on invalid access)
    int stackFixedArrays;    // when true, eligible local T[N] use stack storage
    int emitLoc;             // when true, emit `tua_set_loc` calls for runtime error reporting

    // Generic diagnostics: instantiation backtrace for monomorphization errors.
    List* genericInstStack; // List<GenericInstFrame*>
} Compiler;

// Returns true if the current function should box the local binding `name` (escape analysis).
int compilerShouldBoxLocal(Compiler* compiler, const char* name, int nameLen);

typedef struct GenericInstFrame {
    const char* file; // may be NULL
    int line;
    int col;
    char* pretty; // e.g. "foo<int, string>"
} GenericInstFrame;

#ifndef TUA_GENERIC_INST_MAX_DEPTH
#define TUA_GENERIC_INST_MAX_DEPTH 64
#endif

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
    List* funcTypes; // List<LLVMTypeRef>, nesting level -> expected closure call signature (env + args)
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
    ALIAS_TRAIT,
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
    // Collected method signatures (from `struct` body and `impl` blocks).
    // Elements are FuncStmt* (unmangled AST methods without injected `this`).
    List* methods;
} StructInfo;

StructInfo* compilerFindStruct(Compiler* compiler, const char* name, int length);

typedef struct TraitInfo {
    char* name;
    int nameLength;
    TraitStmt* decl;
    // v2-ish infrastructure (used for "no `dyn`" trait objects): per-trait object and vtable types.
    // Object type name: `<TraitName>__obj`, layout: { i8* data, i8* vtable } (opaque vtable ptr).
    LLVMTypeRef objType;
    // Vtable type name: `<TraitName>__vtable`, layout: { i8* drop, i8* m0, i8* m1, ... }.
    LLVMTypeRef vtableType;
} TraitInfo;

TraitInfo* compilerFindTrait(Compiler* compiler, const char* name, int length);
TraitInfo* compilerResolveTraitByToken(Compiler* compiler, const Token* name);
LLVMTypeRef compilerGetTraitObjType(Compiler* compiler, TraitInfo* trait);
LLVMTypeRef compilerGetTraitVtableType(Compiler* compiler, TraitInfo* trait);

typedef struct GenericSubst {
    const char* name;
    int nameLen;
    Type* type; // concrete type AST
    // Optional: qualified bound trait name for this type parameter (v1 bounds).
    const char* boundTraitName;
    int boundTraitNameLen;
} GenericSubst;

typedef struct GenericFuncTemplate {
    char* qualifiedName;
    int qualifiedNameLen;
    FuncStmt* decl; // AST function template (with typeParams)
    const char* filePath; // for diagnostics
    const char* modulePrefix;
    int modulePrefixLen;
    List* aliases; // List<SymbolAlias*> for the template's module
} GenericFuncTemplate;

typedef struct TraitImplPair {
    char* traitName;
    int traitNameLen;
    char* targetName;
    int targetNameLen;
} TraitImplPair;

void compilerRegisterGenericFuncTemplate(
    Compiler* compiler,
    FuncStmt* decl,
    const char* qualifiedName,
    int qualifiedNameLen,
    const char* filePath,
    const char* modulePrefix,
    int modulePrefixLen,
    List* aliases
);

GenericFuncTemplate* compilerFindGenericFuncTemplate(Compiler* compiler, const char* qualifiedName, int qualifiedNameLen);

// If `type` is a type parameter name in the current monomorphization context, return its substituted concrete type.
// Otherwise return `type` unchanged.
Type* compilerResolveGenericType(Compiler* compiler, Type* type);
GenericSubst* compilerFindGenericSubst(Compiler* compiler, const char* name, int nameLen);

// Instantiate a generic function template with explicit type arguments.
// Returns the monomorphized LLVM function value, or NULL if no template is registered for `baseName`.
LLVMValueRef compilerInstantiateGenericFunc(Compiler* compiler, const char* baseName, int baseNameLen, List* typeArgs, const Token* callSite);

void compilerRecordTraitImplPair(Compiler* compiler, const char* traitName, int traitLen, const char* targetName, int targetLen);
int compilerHasTraitImplPair(Compiler* compiler, const char* traitName, int traitLen, const char* targetName, int targetLen);

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
void compileTraitStmt(Compiler* compiler, TraitStmt* stmt);
void compileTraitImplStmt(Compiler* compiler, TraitImplStmt* stmt);
void compileObjectStmt(Compiler* compiler, ObjectStmt* stmt);
void compileEnumStmt(Compiler* compiler, EnumStmt* stmt);
void initCompiler(Compiler* compiler);
void compilerEmitDropForBlockVars(Compiler* compiler, Block* block);
void compilerEmitDropForCurrentFunctionScopes(Compiler* compiler);

	LLVMTypeRef compilerGetClosureType(Compiler* compiler);
	LLVMTypeRef compilerGetMapType(Compiler* compiler);
	LLVMTypeRef compilerGetArrayType(Compiler* compiler);
	LLVMTypeRef compilerGetBytesType(Compiler* compiler);
    LLVMTypeRef compilerGetSliceType(Compiler* compiler, LLVMTypeRef elemType);
	LLVMTypeRef compilerGetTuaValueType(Compiler* compiler);
	LLVMTypeRef compilerGetOptionType(Compiler* compiler, LLVMTypeRef inner);
LLVMValueRef compilerGetOrCreateStructDrop(Compiler* compiler, StructInfo* info);
LLVMValueRef compilerGetOrCreateBoxDropFn(
    Compiler* compiler,
    LLVMTypeRef valueType,
    Type* astType,
    int isTraitObj,
    const char* traitName,
    int traitNameLen
);
void compilerRegisterClosureSig(Compiler* compiler, const char* name, int nameLen, LLVMTypeRef funcType);
LLVMTypeRef compilerFindClosureSig(Compiler* compiler, const char* name, int nameLen);
LLVMTypeRef compilerClosureSigFromType(Compiler* compiler, Type* type);
void compilerRegisterClosureReturnSig(Compiler* compiler, const char* name, int nameLen, LLVMTypeRef funcType);
LLVMTypeRef compilerFindClosureReturnSig(Compiler* compiler, const char* name, int nameLen);
LLVMTypeRef compilerFindClosureReturnSigAt(Compiler* compiler, const char* name, int nameLen, int level);

#endif
