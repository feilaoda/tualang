; ModuleID = 'tua_module'
source_filename = "tua_module"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-apple-darwin23.6.0"

@tua_file.29 = private unnamed_addr constant [77 x i8] c"/Users/feilaoda/workspace/lowcodecloud/Tuajit/tulang/tests/perf/for_loop.tua\00", align 1
@fmt = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1

define noundef i32 @main() local_unnamed_addr {
entry:
  tail call void @tua_set_loc(ptr nonnull @tua_file.29, i32 2, i32 13)
  tail call void @tua_set_loc(ptr nonnull @tua_file.29, i32 3, i32 13)
  tail call void @tua_set_loc(ptr nonnull @tua_file.29, i32 17, i32 18)
  tail call void @tua_set_loc(ptr nonnull @tua_file.29, i32 17, i32 17)
  tail call void @tua_set_loc(ptr nonnull @tua_file.29, i32 17, i32 14)
  tail call void @tua_set_loc(ptr nonnull @tua_file.29, i32 17, i32 16)
  %call = tail call i32 @_Users_feilaoda_workspace_lowcodecloud_Tuajit_tulang_tests_perf_for_loop_tua__loop(i32 10, i32 1)
  %0 = tail call i32 (ptr, ...) @printf(ptr nonnull dereferenceable(1) @fmt, i32 %call)
  ret i32 0
}

declare void @tua_set_loc(ptr, i32, i32) local_unnamed_addr

define i32 @_Users_feilaoda_workspace_lowcodecloud_Tuajit_tulang_tests_perf_for_loop_tua__loop(i32 %0, i32 %1) local_unnamed_addr {
entry:
  %arr_buf = alloca [10000 x i32], align 4
  %arr_buf39 = bitcast ptr %arr_buf to ptr
  tail call void @tua_set_loc(ptr nonnull @tua_file.29, i32 5, i32 25)
  call void @llvm.experimental.memset.pattern.p0.i32.i64(ptr nonnull align 4 %arr_buf39, i32 1, i64 10000, i1 false)
  tail call void @tua_set_loc(ptr nonnull @tua_file.29, i32 6, i32 15)
  tail call void @tua_set_loc(ptr nonnull @tua_file.29, i32 6, i32 19)
  tail call void @tua_set_loc(ptr nonnull @tua_file.29, i32 6, i32 18)
  tail call void @tua_set_loc(ptr nonnull @tua_file.29, i32 6, i32 20)
  br label %loop.body

loop.body:                                        ; preds = %entry, %loop.end8
  %indvars.iv = phi i64 [ 0, %entry ], [ %indvars.iv.next, %loop.end8 ]
  tail call void @tua_set_loc(ptr nonnull @tua_file.29, i32 7, i32 17)
  tail call void @tua_set_loc(ptr nonnull @tua_file.29, i32 7, i32 19)
  %ep4 = getelementptr inbounds nuw i32, ptr %arr_buf, i64 %indvars.iv
  %av = load i32, ptr %ep4, align 4
  tail call void @tua_set_loc(ptr nonnull @tua_file.29, i32 8, i32 17)
  tail call void @tua_set_loc(ptr nonnull @tua_file.29, i32 8, i32 21)
  tail call void @tua_set_loc(ptr nonnull @tua_file.29, i32 8, i32 20)
  tail call void @tua_set_loc(ptr nonnull @tua_file.29, i32 8, i32 22)
  br label %loop.body6

loop.end:                                         ; preds = %loop.end8
  tail call void @tua_set_loc(ptr nonnull @tua_file.29, i32 13, i32 12)
  tail call void @tua_set_loc(ptr nonnull @tua_file.29, i32 13, i32 14)
  %sext25 = sext i32 %1 to i64
  %ep26 = getelementptr inbounds i32, ptr %arr_buf, i64 %sext25
  %av27 = load i32, ptr %ep26, align 4
  ret i32 %av27

loop.body6:                                       ; preds = %loop.body, %loop.body6
  %j.037 = phi i32 [ 0, %loop.body ], [ %inc14, %loop.body6 ]
  %acc.036 = phi i32 [ %av, %loop.body ], [ %add, %loop.body6 ]
  tail call void @tua_set_loc(ptr nonnull @tua_file.29, i32 9, i32 11)
  tail call void @tua_set_loc(ptr nonnull @tua_file.29, i32 9, i32 21)
  tail call void @tua_set_loc(ptr nonnull @tua_file.29, i32 9, i32 17)
  tail call void @tua_set_loc(ptr nonnull @tua_file.29, i32 9, i32 25)
  tail call void @tua_set_loc(ptr nonnull @tua_file.29, i32 9, i32 23)
  tail call void @tua_set_loc(ptr nonnull @tua_file.29, i32 9, i32 27)
  %div = sdiv i32 %j.037, %0
  %add = add i32 %div, %acc.036
  %inc14 = add nuw nsw i32 %j.037, 1
  tail call void @tua_set_loc(ptr nonnull @tua_file.29, i32 8, i32 21)
  tail call void @tua_set_loc(ptr nonnull @tua_file.29, i32 8, i32 20)
  tail call void @tua_set_loc(ptr nonnull @tua_file.29, i32 8, i32 22)
  %exitcond.not = icmp eq i32 %inc14, 10000
  br i1 %exitcond.not, label %loop.end8, label %loop.body6

loop.end8:                                        ; preds = %loop.body6
  tail call void @tua_set_loc(ptr nonnull @tua_file.29, i32 11, i32 7)
  tail call void @tua_set_loc(ptr nonnull @tua_file.29, i32 11, i32 9)
  tail call void @tua_set_loc(ptr nonnull @tua_file.29, i32 11, i32 18)
  tail call void @tua_set_loc(ptr nonnull @tua_file.29, i32 11, i32 14)
  tail call void @tua_set_loc(ptr nonnull @tua_file.29, i32 11, i32 20)
  %add18 = add i32 %add, %1
  store i32 %add18, ptr %ep4, align 4
  %indvars.iv.next = add nuw nsw i64 %indvars.iv, 1
  tail call void @tua_set_loc(ptr nonnull @tua_file.29, i32 6, i32 19)
  tail call void @tua_set_loc(ptr nonnull @tua_file.29, i32 6, i32 18)
  tail call void @tua_set_loc(ptr nonnull @tua_file.29, i32 6, i32 20)
  %exitcond41.not = icmp eq i64 %indvars.iv.next, 10000
  br i1 %exitcond41.not, label %loop.end, label %loop.body
}

; Function Attrs: nofree nounwind
declare noundef i32 @printf(ptr noundef readonly captures(none), ...) local_unnamed_addr #0

; Function Attrs: nocallback nofree nounwind willreturn memory(argmem: write)
declare void @llvm.experimental.memset.pattern.p0.i32.i64(ptr writeonly captures(none), i32, i64, i1 immarg) #1

attributes #0 = { nofree nounwind }
attributes #1 = { nocallback nofree nounwind willreturn memory(argmem: write) }
