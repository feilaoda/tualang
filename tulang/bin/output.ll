; ModuleID = 'tua_module'
source_filename = "tua_module"

@kstr = private unnamed_addr constant [2 x i8] c"a\00", align 1
@str = private unnamed_addr constant [8 x i8] c"missing\00", align 1
@fmt = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@str.1 = private unnamed_addr constant [8 x i8] c"missing\00", align 1
@fmt.2 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@fmt.3 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1

define i32 @main() {
entry:
  %_Users_feilaoda_workspace_lowcodecloud_Tuajit_tulang_examples_map_get_missing_debug_tua__m = alloca ptr, align 8
  %map = call ptr @tua_map_new()
  call void @tua_map_set(ptr %map, { i32, i64 } { i32 5, i64 ptrtoint (ptr @kstr to i64) }, { i32, i64 } { i32 1, i64 1 })
  call void @tua_map_set(ptr %map, { i32, i64 } { i32 2, i64 2 }, { i32, i64 } { i32 1, i64 3 })
  store ptr %map, ptr %_Users_feilaoda_workspace_lowcodecloud_Tuajit_tulang_examples_map_get_missing_debug_tua__m, align 8
  %_Users_feilaoda_workspace_lowcodecloud_Tuajit_tulang_examples_map_get_missing_debug_tua__a = alloca ptr, align 8
  %load = load ptr, ptr %_Users_feilaoda_workspace_lowcodecloud_Tuajit_tulang_examples_map_get_missing_debug_tua__m, align 8
  %mget = call { i32, i64 } @tua_map_get(ptr %load, { i32, i64 } { i32 5, i64 ptrtoint (ptr @str to i64) })
  %s = call ptr @tua_value_to_string({ i32, i64 } %mget)
  store ptr %s, ptr %_Users_feilaoda_workspace_lowcodecloud_Tuajit_tulang_examples_map_get_missing_debug_tua__a, align 8
  %load1 = load ptr, ptr %_Users_feilaoda_workspace_lowcodecloud_Tuajit_tulang_examples_map_get_missing_debug_tua__a, align 8
  %l_null = icmp eq ptr %load1, null
  %either_null = or i1 %l_null, true
  %both_null = and i1 %l_null, true
  br i1 %either_null, label %str.null, label %str.cmp

str.null:                                         ; preds = %entry
  br label %str.cont

str.cmp:                                          ; preds = %entry
  %strcmp = call i32 @strcmp(ptr %load1, ptr null)
  %streq = icmp eq i32 %strcmp, 0
  br label %str.cont

str.cont:                                         ; preds = %str.cmp, %str.null
  %str_phi = phi i1 [ %both_null, %str.null ], [ %streq, %str.cmp ]
  %zext_printf = zext i1 %str_phi to i32
  %0 = call i32 (ptr, ...) @printf(ptr @fmt, i32 %zext_printf)
  %mval = load ptr, ptr %_Users_feilaoda_workspace_lowcodecloud_Tuajit_tulang_examples_map_get_missing_debug_tua__m, align 8
  %mget2 = call { i32, i64 } @tua_map_get(ptr %mval, { i32, i64 } { i32 5, i64 ptrtoint (ptr @str.1 to i64) })
  %mok32 = call i32 @tua_map_has(ptr %mval, { i32, i64 } { i32 5, i64 ptrtoint (ptr @str.1 to i64) })
  %mok = trunc i32 %mok32 to i1
  %mv = extractvalue { i32, i64 } %mget2, 0
  %_Users_feilaoda_workspace_lowcodecloud_Tuajit_tulang_examples_map_get_missing_debug_tua__v2 = alloca ptr, align 8
  store i32 %mv, ptr %_Users_feilaoda_workspace_lowcodecloud_Tuajit_tulang_examples_map_get_missing_debug_tua__v2, align 4
  %mv3 = extractvalue { i32, i64 } %mget2, 1
  %trunc = trunc i64 %mv3 to i1
  %_Users_feilaoda_workspace_lowcodecloud_Tuajit_tulang_examples_map_get_missing_debug_tua__ok2 = alloca i1, align 1
  store i1 %trunc, ptr %_Users_feilaoda_workspace_lowcodecloud_Tuajit_tulang_examples_map_get_missing_debug_tua__ok2, align 1
  %load4 = load i1, ptr %_Users_feilaoda_workspace_lowcodecloud_Tuajit_tulang_examples_map_get_missing_debug_tua__ok2, align 1
  %zext_printf5 = zext i1 %load4 to i32
  %1 = call i32 (ptr, ...) @printf(ptr @fmt.2, i32 %zext_printf5)
  %load6 = load ptr, ptr %_Users_feilaoda_workspace_lowcodecloud_Tuajit_tulang_examples_map_get_missing_debug_tua__v2, align 8
  %l_null7 = icmp eq ptr %load6, null
  %either_null8 = or i1 %l_null7, true
  %both_null9 = and i1 %l_null7, true
  br i1 %either_null8, label %str.null10, label %str.cmp11

str.null10:                                       ; preds = %str.cont
  br label %str.cont12

str.cmp11:                                        ; preds = %str.cont
  %strcmp13 = call i32 @strcmp(ptr %load6, ptr null)
  %streq14 = icmp eq i32 %strcmp13, 0
  br label %str.cont12

str.cont12:                                       ; preds = %str.cmp11, %str.null10
  %str_phi15 = phi i1 [ %both_null9, %str.null10 ], [ %streq14, %str.cmp11 ]
  %zext_printf16 = zext i1 %str_phi15 to i32
  %2 = call i32 (ptr, ...) @printf(ptr @fmt.3, i32 %zext_printf16)
  ret i32 0
}

declare ptr @tua_map_new()

declare void @tua_map_set(ptr, { i32, i64 }, { i32, i64 })

declare { i32, i64 } @tua_map_get(ptr, { i32, i64 })

declare ptr @tua_value_to_string({ i32, i64 })

declare i32 @printf(ptr, ...)

declare i32 @strcmp(ptr, ptr)

declare i32 @tua_map_has(ptr, { i32, i64 })
