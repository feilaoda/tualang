; ModuleID = 'tua_module'
source_filename = "tua_module"

@enum_variant = private unnamed_addr constant [6 x i8] c"Admin\00", align 1
@enum_variant.1 = private unnamed_addr constant [5 x i8] c"User\00", align 1
@enum_variant.2 = private unnamed_addr constant [6 x i8] c"Guest\00", align 1
@enum_unknown = private unnamed_addr constant [8 x i8] c"Unknown\00", align 1
@fmt = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@fmt.3 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@fmt.4 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@fmt.5 = private unnamed_addr constant [4 x i8] c"%s\0A\00", align 1
@fmt.6 = private unnamed_addr constant [4 x i8] c"%s\0A\00", align 1
@fmt.7 = private unnamed_addr constant [4 x i8] c"%s\0A\00", align 1

define i32 @main() {
entry:
  %0 = call i32 (ptr, ...) @printf(ptr @fmt, i32 10)
  %1 = call i32 (ptr, ...) @printf(ptr @fmt.3, i32 11)
  %2 = call i32 (ptr, ...) @printf(ptr @fmt.4, i32 20)
  %call = call ptr @RoleType__toString(i32 10)
  %3 = call i32 (ptr, ...) @printf(ptr @fmt.5, ptr %call)
  %call1 = call ptr @RoleType__toString(i32 11)
  %4 = call i32 (ptr, ...) @printf(ptr @fmt.6, ptr %call1)
  %call2 = call ptr @RoleType__toString(i32 20)
  %5 = call i32 (ptr, ...) @printf(ptr @fmt.7, ptr %call2)
  ret i32 0
}

define ptr @RoleType__toString(i32 %0) {
entry:
  switch i32 %0, label %default [
    i32 10, label %case
    i32 11, label %case1
    i32 20, label %case2
  ]

default:                                          ; preds = %entry
  ret ptr @enum_unknown

case:                                             ; preds = %entry
  ret ptr @enum_variant

case1:                                            ; preds = %entry
  ret ptr @enum_variant.1

case2:                                            ; preds = %entry
  ret ptr @enum_variant.2
}

declare i32 @printf(ptr, ...)
