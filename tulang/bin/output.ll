; ModuleID = 'tua_module'
source_filename = "tua_module"

@fmt = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1

define i32 @main() {
entry:
  %a = alloca i32, align 4
  store i32 0, ptr %a, align 4
  %b = alloca i32, align 4
  store i32 2, ptr %b, align 4
  %c = alloca i32, align 4
  store i32 30, ptr %c, align 4
  %load = load i32, ptr %b, align 4
  %mul = mul i32 %load, 2
  store i32 %mul, ptr %c, align 4
  store i32 0, ptr %a, align 4
  %i = alloca i32, align 4
  store i32 0, ptr %i, align 4
  br label %loop.cond

loop.cond:                                        ; preds = %loop.inc, %entry
  %load1 = load i32, ptr %i, align 4
  %icmp_le = icmp sle i32 %load1, 100000000
  br i1 %icmp_le, label %loop.body, label %loop.end

loop.body:                                        ; preds = %loop.cond
  %load2 = load i32, ptr %a, align 4
  %load3 = load i32, ptr %i, align 4
  %add = add i32 %load2, %load3
  store i32 %add, ptr %a, align 4
  br label %loop.inc

loop.inc:                                         ; preds = %loop.body
  %i.val = load i32, ptr %i, align 4
  %inc = add i32 %i.val, 1
  store i32 %inc, ptr %i, align 4
  br label %loop.cond

loop.end:                                         ; preds = %loop.cond
  %print_val = load i32, ptr %a, align 4
  %0 = call i32 (ptr, ...) @printf(ptr @fmt, i32 %print_val)
  ret i32 0
}

declare i32 @printf(ptr, ...)
