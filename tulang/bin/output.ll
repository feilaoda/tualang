; ModuleID = 'tua_module'
source_filename = "tua_module"

@tua_file = private unnamed_addr constant [33 x i8] c"/private/tmp/long_param_test.tua\00", align 1
@tua_file.1 = private unnamed_addr constant [33 x i8] c"/private/tmp/long_param_test.tua\00", align 1
@tua_file.2 = private unnamed_addr constant [33 x i8] c"/private/tmp/long_param_test.tua\00", align 1
@tua_file.3 = private unnamed_addr constant [33 x i8] c"/private/tmp/long_param_test.tua\00", align 1
@tua_file.4 = private unnamed_addr constant [33 x i8] c"/private/tmp/long_param_test.tua\00", align 1
@tua_file.5 = private unnamed_addr constant [33 x i8] c"/private/tmp/long_param_test.tua\00", align 1
@fmt = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@tua_file.6 = private unnamed_addr constant [33 x i8] c"/private/tmp/long_param_test.tua\00", align 1
@tua_file.7 = private unnamed_addr constant [33 x i8] c"/private/tmp/long_param_test.tua\00", align 1
@tua_file.8 = private unnamed_addr constant [33 x i8] c"/private/tmp/long_param_test.tua\00", align 1
@tua_file.9 = private unnamed_addr constant [33 x i8] c"/private/tmp/long_param_test.tua\00", align 1

define i32 @main() {
entry:
  %_private_tmp_long_param_test_tua__r = alloca i32, align 4
  %_private_tmp_long_param_test_tua__v = alloca i64, align 8
  call void @tua_set_loc(ptr @tua_file.1, i32 2, i32 14)
  store i64 500000500000, ptr %_private_tmp_long_param_test_tua__v, align 4
  call void @tua_set_loc(ptr @tua_file.2, i32 3, i32 13)
  call void @tua_set_loc(ptr @tua_file.3, i32 3, i32 12)
  %load = load i64, ptr %_private_tmp_long_param_test_tua__v, align 4
  %call = call i64 @_private_tmp_long_param_test_tua__id(i64 %load)
  %trunc = trunc i64 %call to i32
  store i32 %trunc, ptr %_private_tmp_long_param_test_tua__r, align 4
  call void @tua_set_loc(ptr @tua_file.4, i32 4, i32 10)
  call void @tua_set_loc(ptr @tua_file.5, i32 4, i32 9)
  %load1 = load i32, ptr %_private_tmp_long_param_test_tua__r, align 4
  %0 = call i32 (ptr, ...) @printf(ptr @fmt, i32 %load1)
  call void @tua_set_loc(ptr @tua_file.6, i32 5, i32 14)
  call void @tua_set_loc(ptr @tua_file.7, i32 5, i32 10)
  call void @tua_set_loc(ptr @tua_file.8, i32 5, i32 8)
  %load2 = load i32, ptr %_private_tmp_long_param_test_tua__r, align 4
  call void @tua_set_loc(ptr @tua_file.9, i32 5, i32 13)
  %load3 = load i64, ptr %_private_tmp_long_param_test_tua__v, align 4
  %sext = sext i32 %load2 to i64
  %icmp_eq = icmp eq i64 %sext, %load3
  br i1 %icmp_eq, label %assert.ok, label %assert.fail

assert.ok:                                        ; preds = %entry
  br label %assert.cont

assert.fail:                                      ; preds = %entry
  call void @tua_assert_fail(ptr null, i32 5)
  unreachable

assert.cont:                                      ; preds = %assert.ok
  ret i32 0
}

define i64 @_private_tmp_long_param_test_tua__id(i64 %0) {
entry:
  %x = alloca i64, align 8
  store i64 %0, ptr %x, align 4
  br label %tailrecurse

tailrecurse:                                      ; preds = %entry
  call void @tua_set_loc(ptr @tua_file, i32 1, i32 29)
  %load = load i64, ptr %x, align 4
  ret i64 %load
}

declare void @tua_set_loc(ptr, i32, i32)

declare i32 @printf(ptr, ...)

declare void @tua_assert_fail(ptr, i32)
