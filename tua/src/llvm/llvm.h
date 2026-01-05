#ifndef _LLVM_H_
#define _LLVM_H_
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "list.h"
#include "compiler.h"

#include <llvm-c/Core.h>
#include <llvm-c/Analysis.h>

#define emitDebug(...) debug(__VA_ARGS__)


typedef struct VariableRef {
    const char* name;
    int length;
    LLVMValueRef value;
    LLVMTypeRef type;
    // For pointer-like values (e.g. Ref<T>), track the pointee type explicitly.
    // Needed for LLVM opaque pointers where `LLVMGetElementType(ptr)` is unreliable.
    LLVMTypeRef pointeeType;
    // Best-effort semantic type kind (for signed/unsigned codegen decisions).
    TypeKind typeKind;
    const char* typeName;
    int typeNameLength;
    int isConst;
    int isBorrowed; // non-owning view; must not be dropped (map/array)
    int isGlobal;
    int isBoxed;
    int boxOwns; // when boxed, true if this binding owns the box storage (must dec/free at scope end)
    LLVMTypeRef boxPtrType; // T* for boxed variables; slot stores T*

    // Optional: container type tags for pointer-like runtime types (LLVM opaque pointers can't distinguish).
    int isMap;
    int isBytes;
    int isSlice;
    LLVMTypeRef sliceElemType;
    TypeKind sliceElemKind;
    int isTraitObj;
    const char* traitName;
    int traitNameLength;

    // Optional: typed map metadata for `map<K,V>` variables/params.
    int isTypedMap;
    LLVMTypeRef mapKeyType;
    LLVMTypeRef mapValueType;
    TypeKind mapKeyKind;
    TypeKind mapValueKind;
    // When V is a named type, record whether it is the builtin `map` to disambiguate
    // from struct names under LLVM opaque pointers.
    int mapValueIsMap;
    // Optional: when V is a struct type, keep its name for field/method resolution on refs from map.get()/getMut().
    const char* mapValueTypeName;
    int mapValueTypeNameLength;

    // Optional: array metadata for `T[]` / `T[N]` variables/params.
    int isArray;
    LLVMTypeRef arrayElemType;
    TypeKind arrayElemKind;
    int64_t arrayFixedLen; // -1 => dynamic

    // Optional: fast-path metadata for stack-backed fixed arrays.
    int isStackArray;
    LLVMValueRef stackArrayData; // elemTy* (only valid when isStackArray && arrayFixedLen>=0)

    // Optional: when this variable is typed as a generic type parameter (e.g. `T`) in a monomorphized
    // generic function body, keep the original type param name and its bound trait (if any) so that
    // trait static dispatch can be resolved consistently.
    const char* genericParamName;
    int genericParamNameLength;
    const char* genericBoundTraitName;
    int genericBoundTraitNameLength;
}VariableRef;


VariableRef findVariable(List* variables, const char* name);
VariableRef findVariableExpr(Compiler* compiler, Expr* expr);
VariableRef findVariableWithLength(List* variables, const char* name, int length);

LLVMValueRef llvmCoerceToBool(Compiler* compiler, LLVMValueRef value);

void emitVarStmt(Compiler* compiler, VarStmt* stmt);
void emitForStmt(Compiler* compiler, ForStmt* stmt);
void emitWhileStmt(Compiler* compiler, WhileStmt* stmt);
void emitDoWhileStmt(Compiler* compiler, DoWhileStmt* stmt);
void emitBreakStmt(Compiler* compiler);
void emitContinueStmt(Compiler* compiler);
void llvmPushLoop(Compiler* compiler, LLVMBasicBlockRef breakTarget, LLVMBasicBlockRef continueTarget);
void llvmPopLoop(Compiler* compiler);
void emitLabelStmt(Compiler* compiler, LabelStmt* stmt);
void emitGotoStmt(Compiler* compiler, GotoStmt* stmt);
void emitIfStmt(Compiler* compiler, IfStmt* stmt);
void emitForInStmt(Compiler* compiler, ForInStmt* stmt);
// LLVMValueRef emitExpr(Compiler* compiler, Expr* expr);
LLVMValueRef emitCallExpr(Compiler* compiler, CallExpr* expr);
LLVMValueRef emitLiteralExpr(Compiler* compiler, LiteralExpr* expr);
LLVMValueRef emitAssignExpr(Compiler* compiler, AssignExpr* expr);
LLVMValueRef emitUnaryExpr(Compiler* compiler, UnaryExpr* expr);
LLVMValueRef emitVariableExpr(Compiler* compiler, VariableExpr* expr);
LLVMValueRef emitBinaryExpr(Compiler* compiler, BinaryExpr* expr);
LLVMValueRef emitGetExpr(Compiler* compiler, GetExpr* expr);
LLVMValueRef emitSetExpr(Compiler* compiler, SetExpr* expr);
LLVMValueRef emitCastExpr(Compiler* compiler, CastExpr* expr);
LLVMValueRef emitPrefixExpr(Compiler* compiler, PrefixExpr* expr);
LLVMValueRef emitPostfixExpr(Compiler* compiler, PostfixExpr* expr);
	LLVMValueRef emitLambdaExpr(Compiler* compiler, LambdaExpr* expr);
	LLVMValueRef emitMapLiteralExpr(Compiler* compiler, MapLiteralExpr* expr);
	LLVMValueRef emitArrayLiteralExpr(Compiler* compiler, ArrayLiteralExpr* expr);
	LLVMValueRef emitBraceLiteralExpr(Compiler* compiler, BraceLiteralExpr* expr);
	LLVMValueRef emitIndexExpr(Compiler* compiler, IndexExpr* expr);
	LLVMValueRef emitIndexSetExpr(Compiler* compiler, IndexSetExpr* expr);
	LLVMValueRef emitStructInitExpr(Compiler* compiler, StructInitExpr* expr);

// Closure capture analysis helper.
// Returns a Token-set (List<Token*>) that must be freed by the caller:
// - free each Token* element
// - then listFree(list)
List* compilerComputeLambdaFreeNames(Compiler* compiler, LambdaExpr* expr);

#endif
