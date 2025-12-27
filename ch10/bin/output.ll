; ModuleID = 'tua_module'
source_filename = "tua_module"

@fmt = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1

define i32 @main() {
entry:
  %a = alloca i32, align 4
  store i32 10, ptr %a, align 4
  %print_val = load i32, ptr %a, align 4
  %0 = call i32 (ptr, ...) @printf(ptr @fmt, i32 %print_val)
  ret i32 0
}

declare i32 @printf(ptr, ...)
