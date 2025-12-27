; ModuleID = 'tua_module'
source_filename = "tua_module"

@str = private unnamed_addr constant [12 x i8] c"hello world\00", align 1
@fmt = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@fmt.1 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@fmt.2 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1

define i32 @main() {
entry:
  %a = alloca i32, align 4
  store i32 10, ptr %a, align 4
  %s = alloca ptr, align 8
  store ptr @str, ptr %s, align 8
  %load = load i32, ptr %a, align 4
  %icmp_gt = icmp sgt i32 %load, 5
  br i1 %icmp_gt, label %if.then, label %if.else

if.then:                                          ; preds = %entry
  %load1 = load i32, ptr %a, align 4
  %0 = call i32 (ptr, ...) @printf(ptr @fmt, i32 %load1)
  br label %if.end

if.else:                                          ; preds = %entry
  %1 = call i32 (ptr, ...) @printf(ptr @fmt.1, i32 0)
  br label %if.end

if.end:                                           ; preds = %if.else, %if.then
  %i = alloca i32, align 4
  store i32 0, ptr %i, align 4
  br label %loop.cond

loop.cond:                                        ; preds = %loop.inc, %if.end
  %load2 = load i32, ptr %i, align 4
  %icmp_lt = icmp slt i32 %load2, 3
  br i1 %icmp_lt, label %loop.body, label %loop.end

loop.body:                                        ; preds = %loop.cond
  %load3 = load i32, ptr %a, align 4
  %load4 = load i32, ptr %i, align 4
  %add = add i32 %load3, %load4
  store i32 %add, ptr %a, align 4
  br label %loop.inc

loop.inc:                                         ; preds = %loop.body
  %i.val = load i32, ptr %i, align 4
  %inc = add i32 %i.val, 1
  store i32 %inc, ptr %i, align 4
  br label %loop.cond

loop.end:                                         ; preds = %loop.cond
  %load5 = load i32, ptr %a, align 4
  %2 = call i32 (ptr, ...) @printf(ptr @fmt.2, i32 %load5)
  ret i32 0
}

declare i32 @printf(ptr, ...)
