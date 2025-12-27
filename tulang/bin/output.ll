; ModuleID = 'tua_module'
source_filename = "tua_module"

%A = type { i32 }

@fmt = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@fmt.1 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@fmt.2 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@fmt.3 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1

define i32 @main() {
entry:
  %a = alloca %A, align 8
  %ctor_tmp = alloca %A, align 8
  %field_ptr = getelementptr inbounds nuw %A, ptr %ctor_tmp, i32 0, i32 0
  store i32 1, ptr %field_ptr, align 4
  %ctor = load %A, ptr %ctor_tmp, align 4
  store %A %ctor, ptr %a, align 4
  %b = alloca %A, align 8
  %load = load %A, ptr %a, align 4
  store %A %load, ptr %b, align 4
  %field_ptr1 = getelementptr inbounds nuw %A, ptr %b, i32 0, i32 0
  store i32 2, ptr %field_ptr1, align 4
  %field_ptr2 = getelementptr inbounds nuw %A, ptr %a, i32 0, i32 0
  %field = load i32, ptr %field_ptr2, align 4
  %0 = call i32 (ptr, ...) @printf(ptr @fmt, i32 %field)
  %field_ptr3 = getelementptr inbounds nuw %A, ptr %b, i32 0, i32 0
  %field4 = load i32, ptr %field_ptr3, align 4
  %1 = call i32 (ptr, ...) @printf(ptr @fmt.1, i32 %field4)
  %c = alloca ptr, align 8
  store ptr %a, ptr %c, align 8
  %recv_ptr = load ptr, ptr %c, align 8
  %field_ptr5 = getelementptr inbounds nuw %A, ptr %recv_ptr, i32 0, i32 0
  store i32 3, ptr %field_ptr5, align 4
  %field_ptr6 = getelementptr inbounds nuw %A, ptr %a, i32 0, i32 0
  %field7 = load i32, ptr %field_ptr6, align 4
  %2 = call i32 (ptr, ...) @printf(ptr @fmt.2, i32 %field7)
  %recv_ptr8 = load ptr, ptr %c, align 8
  %field_ptr9 = getelementptr inbounds nuw %A, ptr %recv_ptr8, i32 0, i32 0
  %field10 = load i32, ptr %field_ptr9, align 4
  %3 = call i32 (ptr, ...) @printf(ptr @fmt.3, i32 %field10)
  ret i32 0
}

declare i32 @printf(ptr, ...)
