; ModuleID = 'tua_module'
source_filename = "tua_module"

@str = private unnamed_addr constant [3 x i8] c"hi\00", align 1
@fmt = private unnamed_addr constant [4 x i8] c"%s\0A\00", align 1
@fmt.1 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1

define i32 @main() {
entry:
  %call = call { ptr, i32 } @_Users_feilaoda_workspace_lowcodecloud_Tuajit_tulang_examples_multi_return_string_tua__mix()
  %mv = extractvalue { ptr, i32 } %call, 0
  %_Users_feilaoda_workspace_lowcodecloud_Tuajit_tulang_examples_multi_return_string_tua__s = alloca ptr, align 8
  store ptr %mv, ptr %_Users_feilaoda_workspace_lowcodecloud_Tuajit_tulang_examples_multi_return_string_tua__s, align 8
  %mv1 = extractvalue { ptr, i32 } %call, 1
  %_Users_feilaoda_workspace_lowcodecloud_Tuajit_tulang_examples_multi_return_string_tua__n = alloca i32, align 4
  store i32 %mv1, ptr %_Users_feilaoda_workspace_lowcodecloud_Tuajit_tulang_examples_multi_return_string_tua__n, align 4
  %load = load ptr, ptr %_Users_feilaoda_workspace_lowcodecloud_Tuajit_tulang_examples_multi_return_string_tua__s, align 8
  %0 = call i32 (ptr, ...) @printf(ptr @fmt, ptr %load)
  %load2 = load i32, ptr %_Users_feilaoda_workspace_lowcodecloud_Tuajit_tulang_examples_multi_return_string_tua__n, align 4
  %1 = call i32 (ptr, ...) @printf(ptr @fmt.1, i32 %load2)
  ret i32 0
}

define { ptr, i32 } @_Users_feilaoda_workspace_lowcodecloud_Tuajit_tulang_examples_multi_return_string_tua__mix() {
entry:
  ret { ptr, i32 } { ptr @str, i32 42 }
}

declare i32 @printf(ptr, ...)
